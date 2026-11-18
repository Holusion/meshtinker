#include "meshtinker/gltf/geometry.hpp"

#include <draco/compression/decode.h>
#include <draco/mesh/mesh.h>
#include <meshoptimizer.h>

#include <memory>

namespace meshtinker::gltf {
namespace {

constexpr std::uint64_t heap_limit = std::uint64_t{4} << 20;

} // namespace

GeometryReader::GeometryReader(Document& document, common::Workspace& workspace)
    : document_(document), workspace_(workspace) {}

GeometryReader::~GeometryReader() {
    for (const auto& [accessor, source] : replaced_) {
        document_.setAccessorSource(accessor, source);
    }
    // The document may outlive the decoded bytes: stop cgltf from reading them.
    for (const auto& entry : status_) {
        if (entry.second == Readability::readable && (heap_.count(entry.first) != 0 || scratch_.count(entry.first) != 0)) {
            document_.setDecodedView(entry.first, nullptr);
        }
    }
}

std::uint8_t* GeometryReader::allocate(const std::string& name, std::uint64_t size) {
    if (size <= heap_limit) {
        std::vector<std::uint8_t>& bytes = draco_heap_[name];
        bytes.assign(static_cast<std::size_t>(size), 0);
        return bytes.data();
    }
    common::ScratchMapping& map = draco_scratch_[name];
    map = workspace_.scratch(name, size);
    return map.data();
}

Readability GeometryReader::preparePrimitive(std::size_t mesh, std::size_t primitive) {
    if (!isDraco(document_.data().meshes[mesh].primitives[primitive])) {
        return Readability::readable;
    }
    const auto key = std::make_pair(mesh, primitive);
    if (const auto found = draco_status_.find(key); found != draco_status_.end()) {
        return found->second;
    }
    const Readability status = decodeDraco(mesh, primitive);
    draco_status_[key] = status;
    return status;
}

namespace {

// Writes `count` elements of `attribute` in `component` type, `components` each.
template <typename T>
bool convertAll(const draco::PointAttribute& attribute, std::size_t count, int components, std::uint8_t* out) {
    T* values = reinterpret_cast<T*>(out);
    for (std::size_t i = 0; i < count; ++i) {
        const draco::AttributeValueIndex index = attribute.mapped_index(draco::PointIndex(static_cast<std::uint32_t>(i)));
        if (!attribute.ConvertValue<T>(index, static_cast<std::int8_t>(components), values + i * static_cast<std::size_t>(components))) {
            return false;
        }
    }
    return true;
}

bool convertAttribute(const draco::PointAttribute& attribute, const cgltf_accessor& accessor, std::uint8_t* out) {
    const auto components = static_cast<int>(cgltf_num_components(accessor.type));
    switch (accessor.component_type) {
    case cgltf_component_type_r_8:
        return convertAll<std::int8_t>(attribute, accessor.count, components, out);
    case cgltf_component_type_r_8u:
        return convertAll<std::uint8_t>(attribute, accessor.count, components, out);
    case cgltf_component_type_r_16:
        return convertAll<std::int16_t>(attribute, accessor.count, components, out);
    case cgltf_component_type_r_16u:
        return convertAll<std::uint16_t>(attribute, accessor.count, components, out);
    case cgltf_component_type_r_32u:
        return convertAll<std::uint32_t>(attribute, accessor.count, components, out);
    case cgltf_component_type_r_32f:
        return convertAll<float>(attribute, accessor.count, components, out);
    case cgltf_component_type_invalid:
    case cgltf_component_type_max_enum:
        break;
    }
    return false;
}

template <typename T>
void writeCorners(const draco::Mesh& mesh, std::uint8_t* out) {
    T* indices = reinterpret_cast<T*>(out);
    for (draco::FaceIndex f(0); f < mesh.num_faces(); ++f) {
        const draco::Mesh::Face& face = mesh.face(f);
        for (std::size_t c = 0; c < 3; ++c) {
            indices[static_cast<std::size_t>(f.value()) * 3 + c] = static_cast<T>(face[c].value());
        }
    }
}

} // namespace

Readability GeometryReader::decodeDraco(std::size_t mesh_index, std::size_t primitive_index) {
    const cgltf_data& data = document_.data();
    const cgltf_primitive& primitive = data.meshes[mesh_index].primitives[primitive_index];
    // cgltf reads the extension's attribute IDs as accessor indices: take them from the JSON.
    const Json& extension =
        document_.json()["meshes"][mesh_index]["primitives"][primitive_index]["extensions"]["KHR_draco_mesh_compression"];
    const auto view = extension.find("bufferView");
    const auto attributes = extension.find("attributes");
    if (view == extension.end() || !view->is_number_unsigned() || attributes == extension.end() ||
        !attributes->is_object()) {
        return Readability::draco_failed;
    }
    const common::ByteView compressed = document_.bufferView(view->get<std::size_t>());
    if (compressed.empty()) {
        return Readability::no_data;
    }
    draco::DecoderBuffer buffer;
    buffer.Init(reinterpret_cast<const char*>(compressed.data), compressed.size);
    const auto type = draco::Decoder::GetEncodedGeometryType(&buffer);
    if (!type.ok()) {
        return Readability::draco_failed;
    }
    draco::Decoder decoder;
    std::unique_ptr<draco::PointCloud> cloud;
    const draco::Mesh* mesh = nullptr;
    if (type.value() == draco::TRIANGULAR_MESH) {
        auto decoded = decoder.DecodeMeshFromBuffer(&buffer);
        if (!decoded.ok()) {
            return Readability::draco_failed;
        }
        std::unique_ptr<draco::Mesh> owned = std::move(decoded).value();
        mesh = owned.get();
        cloud = std::move(owned);
    } else if (type.value() == draco::POINT_CLOUD) {
        auto decoded = decoder.DecodePointCloudFromBuffer(&buffer);
        if (!decoded.ok()) {
            return Readability::draco_failed;
        }
        cloud = std::move(decoded).value();
    } else {
        return Readability::draco_failed;
    }

    // Decode everything first, then install it, so a failure leaves no accessor half-replaced.
    std::vector<std::pair<std::size_t, const std::uint8_t*>> installs;
    const std::string prefix = "draco-" + std::to_string(mesh_index) + "-" + std::to_string(primitive_index) + "-";
    for (const auto& item : attributes->items()) {
        if (!item.value().is_number_unsigned()) {
            return Readability::draco_failed;
        }
        const cgltf_accessor* accessor = nullptr;
        for (std::size_t a = 0; a < primitive.attributes_count; ++a) {
            if (primitive.attributes[a].name != nullptr && item.key() == primitive.attributes[a].name) {
                accessor = primitive.attributes[a].data;
            }
        }
        if (accessor == nullptr) {
            continue; // compressed, but the primitive doesn't use it
        }
        const draco::PointAttribute* attribute = cloud->GetAttributeByUniqueId(item.value().get<std::uint32_t>());
        if (attribute == nullptr || accessor->count != cloud->num_points()) {
            return Readability::draco_failed;
        }
        const auto index = static_cast<std::size_t>(accessor - data.accessors);
        const std::uint64_t size = std::uint64_t{accessor->count} * cgltf_calc_size(accessor->type, accessor->component_type);
        std::uint8_t* out = allocate(prefix + item.key(), size);
        if (!convertAttribute(*attribute, *accessor, out)) {
            return Readability::draco_failed;
        }
        installs.emplace_back(index, out);
    }
    if (primitive.indices != nullptr) {
        if (mesh == nullptr || primitive.indices->count != std::size_t{mesh->num_faces()} * 3) {
            return Readability::draco_failed;
        }
        const cgltf_accessor& accessor = *primitive.indices;
        std::uint8_t* out = allocate(prefix + "indices", std::uint64_t{accessor.count} * cgltf_component_size(accessor.component_type));
        switch (accessor.component_type) {
        case cgltf_component_type_r_8u:
            writeCorners<std::uint8_t>(*mesh, out);
            break;
        case cgltf_component_type_r_16u:
            writeCorners<std::uint16_t>(*mesh, out);
            break;
        case cgltf_component_type_r_32u:
            writeCorners<std::uint32_t>(*mesh, out);
            break;
        default:
            return Readability::draco_failed;
        }
        installs.emplace_back(static_cast<std::size_t>(primitive.indices - data.accessors), out);
    }
    for (const auto& [index, bytes] : installs) {
        const cgltf_accessor& accessor = data.accessors[index];
        cgltf_buffer_view& decoded = draco_views_.emplace_back();
        decoded.data = const_cast<std::uint8_t*>(bytes);
        decoded.size = accessor.count * cgltf_calc_size(accessor.type, accessor.component_type);
        draco_view_set_.insert(&decoded);
        const Document::AccessorSource previous =
            document_.setAccessorSource(index, {&decoded, 0, cgltf_calc_size(accessor.type, accessor.component_type)});
        replaced_.emplace(index, previous);
    }
    return Readability::readable;
}

Readability GeometryReader::prepare(const cgltf_accessor& accessor) {
    if (draco_view_set_.count(accessor.buffer_view) != 0) {
        return Readability::readable;
    }
    if (accessor.buffer_view == nullptr && !accessor.is_sparse) {
        // No data at all is zeros, which cgltf returns; a Draco primitive's
        // accessors look like this too, which callers check with isDraco.
        return Readability::readable;
    }
    for (const cgltf_buffer_view* view : {accessor.buffer_view, accessor.is_sparse ? accessor.sparse.indices_buffer_view : nullptr,
                                          accessor.is_sparse ? accessor.sparse.values_buffer_view : nullptr}) {
        if (view == nullptr) {
            continue;
        }
        const Readability status = prepareView(view);
        if (status != Readability::readable) {
            return status;
        }
    }
    return Readability::readable;
}

Readability GeometryReader::prepareView(const cgltf_buffer_view* view) {
    const cgltf_data& data = document_.data();
    const auto index = static_cast<std::size_t>(view - data.buffer_views);
    if (const auto found = status_.find(index); found != status_.end()) {
        return found->second;
    }
    Readability& status = status_[index];
    if (!view->has_meshopt_compression) {
        status = document_.bufferView(index).empty() && view->size > 0 ? Readability::no_data : Readability::readable;
        return status;
    }
    const cgltf_meshopt_compression& compression = view->meshopt_compression;
    const auto buffer = static_cast<std::size_t>(compression.buffer - data.buffers);
    const common::ByteView source = document_.buffer(buffer).sub(compression.offset, compression.size);
    const std::uint64_t size = std::uint64_t{compression.count} * compression.stride;
    if (source.empty() || size == 0) {
        status = Readability::no_data;
        return status;
    }
    std::uint8_t* destination = nullptr;
    if (size <= heap_limit) {
        std::vector<std::uint8_t>& bytes = heap_[index];
        bytes.resize(static_cast<std::size_t>(size));
        destination = bytes.data();
    } else {
        common::ScratchMapping& map = scratch_[index];
        map = workspace_.scratch("meshopt-" + std::to_string(index), size);
        destination = map.data();
    }
    int result = -1;
    switch (compression.mode) {
    case cgltf_meshopt_compression_mode_attributes:
        result = meshopt_decodeVertexBuffer(destination, compression.count, compression.stride, source.data, source.size);
        break;
    case cgltf_meshopt_compression_mode_triangles:
        result = meshopt_decodeIndexBuffer(destination, compression.count, compression.stride, source.data, source.size);
        break;
    case cgltf_meshopt_compression_mode_indices:
        result = meshopt_decodeIndexSequence(destination, compression.count, compression.stride, source.data, source.size);
        break;
    case cgltf_meshopt_compression_mode_invalid:
    case cgltf_meshopt_compression_mode_max_enum:
        break;
    }
    if (result != 0) {
        heap_.erase(index);
        scratch_.erase(index);
        status = Readability::meshopt_failed;
        return status;
    }
    switch (compression.filter) {
    case cgltf_meshopt_compression_filter_octahedral:
        meshopt_decodeFilterOct(destination, compression.count, compression.stride);
        break;
    case cgltf_meshopt_compression_filter_quaternion:
        meshopt_decodeFilterQuat(destination, compression.count, compression.stride);
        break;
    case cgltf_meshopt_compression_filter_exponential:
        meshopt_decodeFilterExp(destination, compression.count, compression.stride);
        break;
    case cgltf_meshopt_compression_filter_color:
        meshopt_decodeFilterColor(destination, compression.count, compression.stride);
        break;
    case cgltf_meshopt_compression_filter_none:
    case cgltf_meshopt_compression_filter_max_enum:
        break;
    }
    document_.setDecodedView(index, destination);
    status = Readability::readable;
    return status;
}

const cgltf_accessor* findAttribute(const cgltf_primitive& primitive, cgltf_attribute_type type, int set) {
    for (std::size_t a = 0; a < primitive.attributes_count; ++a) {
        if (primitive.attributes[a].type == type && primitive.attributes[a].index == set) {
            return primitive.attributes[a].data;
        }
    }
    return nullptr;
}

std::uint64_t cornerCount(const cgltf_primitive& primitive) {
    if (primitive.indices != nullptr) {
        return primitive.indices->count;
    }
    const cgltf_accessor* position = findAttribute(primitive, cgltf_attribute_type_position, 0);
    return position == nullptr ? 0 : position->count;
}

std::uint64_t triangleCount(cgltf_primitive_type type, std::uint64_t corners) {
    switch (type) {
    case cgltf_primitive_type_triangles:
        return corners / 3;
    case cgltf_primitive_type_triangle_strip:
    case cgltf_primitive_type_triangle_fan:
        return corners >= 3 ? corners - 2 : 0;
    default:
        return 0;
    }
}

std::array<std::uint64_t, 3> triangleCorners(cgltf_primitive_type type, std::uint64_t t) {
    switch (type) {
    case cgltf_primitive_type_triangle_strip:
        return {t, t + 1, t + 2};
    case cgltf_primitive_type_triangle_fan:
        return {0, t + 1, t + 2};
    default:
        return {3 * t, 3 * t + 1, 3 * t + 2};
    }
}

} // namespace meshtinker::gltf
