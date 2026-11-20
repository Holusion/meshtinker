#include "meshtinker/gltf/primitive_data.hpp"

#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace meshtinker::gltf {
namespace {

constexpr int unsigned_byte_type = 5121;
constexpr int unsigned_short_type = 5123;
constexpr int unsigned_int_type = 5125;
constexpr int float_type = 5126;
constexpr int array_buffer = 34962;
constexpr int element_array_buffer = 34963;
// glTF reserves each index type's maximum value for primitive restart.
constexpr std::size_t short_index_vertices = std::numeric_limits<std::uint16_t>::max();
constexpr std::uint64_t max_count = std::numeric_limits<std::uint32_t>::max() - 1;

const char* typeName(std::size_t components) {
    switch (components) {
    case 1:
        return "SCALAR";
    case 2:
        return "VEC2";
    case 3:
        return "VEC3";
    default:
        return "VEC4";
    }
}

bool readAttribute(const cgltf_data& data, const cgltf_attribute& attribute, std::size_t vertex_count,
                   common::Workspace& workspace, AttributeData& out) {
    const cgltf_accessor& accessor = *attribute.data;
    out.source_accessor = static_cast<std::size_t>(&accessor - data.accessors);
    const std::size_t components = cgltf_num_components(accessor.type);
    if (accessor.count != vertex_count || components < 1 || components > 4) {
        return false;
    }
    out.name = attribute.name != nullptr ? attribute.name : "";
    out.components = components;
    out.source_component_type = static_cast<int>(accessor.component_type == cgltf_component_type_r_8u    ? unsigned_byte_type
                                                 : accessor.component_type == cgltf_component_type_r_16u ? unsigned_short_type
                                                                                                         : float_type);
    out.values = common::LargeArray<float>(workspace, vertex_count * components);
    return cgltf_accessor_unpack_floats(&accessor, out.values.data(), out.values.size()) == out.values.size();
}

void readCorners(const cgltf_primitive& primitive, std::uint64_t corners, common::LargeArray<std::uint32_t>& out) {
    if (primitive.indices == nullptr) {
        for (std::uint64_t i = 0; i < corners; ++i) {
            out[i] = static_cast<std::uint32_t>(i);
        }
        return;
    }
    if (cgltf_accessor_unpack_indices(primitive.indices, out.data(), sizeof(std::uint32_t), corners) == corners) {
        return;
    }
    for (std::uint64_t i = 0; i < corners; ++i) {
        out[i] = static_cast<std::uint32_t>(cgltf_accessor_read_index(primitive.indices, i));
    }
}

template <typename T>
void narrowInPlace(float* values, std::size_t count) {
    T* out = reinterpret_cast<T*>(values);
    for (std::size_t i = 0; i < count; ++i) {
        // Element i lands at or before where float i was read: forward is safe.
        const float value = values[i];
        out[i] = static_cast<T>(std::clamp(value, 0.0F, static_cast<float>(std::numeric_limits<T>::max())));
    }
}

} // namespace

const AttributeData& TriangleData::position() const {
    return *std::find_if(attributes.begin(), attributes.end(),
                         [](const AttributeData& attribute) { return attribute.name == "POSITION"; });
}

TriangleReadStatus readTriangles(Document& document, GeometryReader& reader, common::Workspace& workspace,
                                 std::size_t mesh, std::size_t primitive, TriangleData& data) {
    const cgltf_primitive& source = document.data().meshes[mesh].primitives[primitive];
    if (source.type != cgltf_primitive_type_triangles && source.type != cgltf_primitive_type_triangle_strip &&
        source.type != cgltf_primitive_type_triangle_fan) {
        return TriangleReadStatus::not_triangles;
    }
    const cgltf_accessor* position = findAttribute(source, cgltf_attribute_type_position, 0);
    if (position == nullptr || position->count > max_count || cornerCount(source) > max_count) {
        return TriangleReadStatus::unsupported;
    }
    if (reader.preparePrimitive(mesh, primitive) != Readability::readable) {
        return TriangleReadStatus::unreadable;
    }
    const auto prepared = [&](const cgltf_accessor* accessor) {
        return accessor == nullptr || reader.prepare(*accessor) == Readability::readable;
    };
    bool readable = prepared(source.indices);
    for (std::size_t a = 0; a < source.attributes_count; ++a) {
        readable = readable && prepared(source.attributes[a].data);
    }
    for (std::size_t t = 0; t < source.targets_count; ++t) {
        for (std::size_t a = 0; a < source.targets[t].attributes_count; ++a) {
            readable = readable && prepared(source.targets[t].attributes[a].data);
        }
    }
    if (!readable) {
        return TriangleReadStatus::unreadable;
    }

    data.vertex_count = position->count;
    if (source.indices != nullptr) {
        data.indices_source = static_cast<std::size_t>(source.indices - document.data().accessors);
    }
    const std::uint64_t corners = cornerCount(source);
    common::LargeArray<std::uint32_t> corner_indices(workspace, static_cast<std::size_t>(corners));
    readCorners(source, corners, corner_indices);
    if (source.type == cgltf_primitive_type_triangles) {
        corner_indices.shrink(static_cast<std::size_t>(corners / 3 * 3));
        data.indices = std::move(corner_indices);
    } else {
        const std::uint64_t triangles = triangleCount(source.type, corners);
        data.indices = common::LargeArray<std::uint32_t>(workspace, static_cast<std::size_t>(triangles * 3));
        std::size_t written = 0;
        for (std::uint64_t t = 0; t < triangles; ++t) {
            std::array<std::uint64_t, 3> c = triangleCorners(source.type, t);
            // Every other strip triangle is wound the other way (spec 3.7.2.1).
            if (source.type == cgltf_primitive_type_triangle_strip && t % 2 == 1) {
                std::swap(c[0], c[1]);
            }
            const std::uint32_t a = corner_indices[c[0]];
            const std::uint32_t b = corner_indices[c[1]];
            const std::uint32_t d = corner_indices[c[2]];
            if (a == b || b == d || a == d) {
                continue;
            }
            data.indices[written++] = a;
            data.indices[written++] = b;
            data.indices[written++] = d;
        }
        data.indices.shrink(written);
    }
    for (const std::uint32_t index : data.indices) {
        if (index >= data.vertex_count) {
            return TriangleReadStatus::unreadable;
        }
    }

    for (std::size_t a = 0; a < source.attributes_count; ++a) {
        AttributeData attribute;
        if (!readAttribute(document.data(), source.attributes[a], data.vertex_count, workspace, attribute)) {
            return TriangleReadStatus::unsupported;
        }
        data.attributes.push_back(std::move(attribute));
    }
    for (std::size_t t = 0; t < source.targets_count; ++t) {
        std::vector<AttributeData> target;
        for (std::size_t a = 0; a < source.targets[t].attributes_count; ++a) {
            AttributeData attribute;
            if (!readAttribute(document.data(), source.targets[t].attributes[a], data.vertex_count, workspace,
                               attribute)) {
                return TriangleReadStatus::unsupported;
            }
            target.push_back(std::move(attribute));
        }
        data.targets.push_back(std::move(target));
    }
    return TriangleReadStatus::ok;
}

TriangleData compactVertices(const TriangleData& source, common::LargeArray<std::uint32_t>&& indices,
                             common::Workspace& workspace) {
    common::LargeArray<std::uint32_t> remap(workspace, source.vertex_count);
    const std::size_t unique =
        meshopt_optimizeVertexFetchRemap(remap.data(), indices.data(), indices.size(), source.vertex_count);
    meshopt_remapIndexBuffer(indices.data(), indices.data(), indices.size(), remap.data());
    TriangleData result;
    result.vertex_count = unique;
    result.indices = std::move(indices);
    result.indices_source = source.indices_source;
    const auto remapAttribute = [&](const AttributeData& attribute) {
        AttributeData out;
        out.name = attribute.name;
        out.components = attribute.components;
        out.source_component_type = attribute.source_component_type;
        out.source_accessor = attribute.source_accessor;
        out.values = common::LargeArray<float>(workspace, unique * attribute.components);
        meshopt_remapVertexBuffer(out.values.data(), attribute.values.data(), source.vertex_count,
                                  attribute.components * sizeof(float), remap.data());
        return out;
    };
    result.attributes.reserve(source.attributes.size());
    for (const AttributeData& attribute : source.attributes) {
        result.attributes.push_back(remapAttribute(attribute));
    }
    for (const std::vector<AttributeData>& target : source.targets) {
        std::vector<AttributeData> out;
        out.reserve(target.size());
        for (const AttributeData& attribute : target) {
            out.push_back(remapAttribute(attribute));
        }
        result.targets.push_back(std::move(out));
    }
    return result;
}

bool readAccessor(const cgltf_data& data, const cgltf_accessor& accessor, const std::string& name,
                  common::Workspace& workspace, AttributeData& out) {
    cgltf_attribute attribute{};
    attribute.name = const_cast<char*>(name.c_str());
    attribute.data = const_cast<cgltf_accessor*>(&accessor);
    return readAttribute(data, attribute, accessor.count, workspace, out);
}

std::size_t addPlainAttribute(Layout& layout, AttributeData& attribute, bool morph_target) {
    const std::size_t count = attribute.values.size() / attribute.components;
    int component_type = float_type;
    std::size_t component_size = sizeof(float);
    if (!morph_target && attribute.name.rfind("JOINTS_", 0) == 0) {
        component_type = attribute.source_component_type == unsigned_byte_type ? unsigned_byte_type : unsigned_short_type;
        component_size = component_type == unsigned_byte_type ? 1 : 2;
        if (component_type == unsigned_byte_type) {
            narrowInPlace<std::uint8_t>(attribute.values.data(), attribute.values.size());
        } else {
            narrowInPlace<std::uint16_t>(attribute.values.data(), attribute.values.size());
        }
    }
    const std::uint64_t byte_length = std::uint64_t{attribute.values.size()} * component_size;
    const common::ByteView bytes{reinterpret_cast<const std::uint8_t*>(attribute.values.data()),
                                 static_cast<std::size_t>(byte_length)};
    const std::size_t view = addView(layout, Json{{"byteLength", byte_length}, {"target", array_buffer}}, bytes);
    Json accessor{{"bufferView", view}, {"componentType", component_type}, {"count", count},
                  {"type", typeName(attribute.components)}};
    copyAccessorProperties(layout, attribute.source_accessor, accessor);
    if (attribute.name == "POSITION") {
        std::array<float, 3> min{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                                 std::numeric_limits<float>::max()};
        std::array<float, 3> max{-min[0], -min[1], -min[2]};
        for (std::size_t v = 0; v < count; ++v) {
            for (std::size_t k = 0; k < 3; ++k) {
                min[k] = std::min(min[k], attribute.values[3 * v + k]);
                max[k] = std::max(max[k], attribute.values[3 * v + k]);
            }
        }
        if (count == 0) {
            min = max = {0, 0, 0};
        }
        accessor["min"] = Json::array({min[0], min[1], min[2]});
        accessor["max"] = Json::array({max[0], max[1], max[2]});
    }
    return addAccessor(layout, std::move(accessor));
}

PrimitiveGeometry addPlainGeometry(Layout& layout, TriangleData& data) {
    PrimitiveGeometry geometry;
    for (AttributeData& attribute : data.attributes) {
        geometry.attributes[attribute.name] = addPlainAttribute(layout, attribute, false);
    }
    if (!data.targets.empty()) {
        geometry.targets = Json::array();
        for (std::vector<AttributeData>& target : data.targets) {
            Json attributes = Json::object();
            for (AttributeData& attribute : target) {
                attributes[attribute.name] = addPlainAttribute(layout, attribute, true);
            }
            geometry.targets.push_back(std::move(attributes));
        }
    }
    const bool short_indices = data.vertex_count <= short_index_vertices;
    std::uint64_t byte_length = std::uint64_t{data.indices.size()} * 4;
    if (short_indices) {
        auto* out = reinterpret_cast<std::uint16_t*>(data.indices.data());
        for (std::size_t i = 0; i < data.indices.size(); ++i) {
            out[i] = static_cast<std::uint16_t>(data.indices[i]);
        }
        byte_length = std::uint64_t{data.indices.size()} * 2;
    }
    const common::ByteView bytes{reinterpret_cast<const std::uint8_t*>(data.indices.data()),
                                 static_cast<std::size_t>(byte_length)};
    const std::size_t view =
        addView(layout, Json{{"byteLength", byte_length}, {"target", element_array_buffer}}, bytes);
    Json indices{{"bufferView", view},
                 {"componentType", short_indices ? unsigned_short_type : unsigned_int_type},
                 {"count", data.indices.size()},
                 {"type", "SCALAR"}};
    copyAccessorProperties(layout, data.indices_source, indices);
    geometry.indices = addAccessor(layout, std::move(indices));
    geometry.mode = 4;
    return geometry;
}

} // namespace meshtinker::gltf
