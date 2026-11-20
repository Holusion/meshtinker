#include "encode.hpp"

#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace meshtinker::compress {
namespace {

constexpr int byte_type = 5120;
constexpr int unsigned_byte_type = 5121;
constexpr int short_type = 5122;
constexpr int unsigned_short_type = 5123;
constexpr int unsigned_int_type = 5125;
constexpr int float_type = 5126;
constexpr int array_buffer = 34962;
constexpr int element_array_buffer = 34963;
constexpr const char* extension_name = "EXT_meshopt_compression";
// EXT_meshopt_compression decoders read version 0 of the vertex codec only.
constexpr int vertex_codec_version = 0;
// glTF reserves each index type's maximum value for primitive restart.
constexpr std::size_t short_index_vertices = std::numeric_limits<std::uint16_t>::max();

std::size_t componentSize(int type) {
    switch (type) {
    case byte_type:
    case unsigned_byte_type:
        return 1;
    case short_type:
    case unsigned_short_type:
        return 2;
    default:
        return 4;
    }
}

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

std::size_t align4(std::size_t n) { return (n + 3) & ~std::size_t{3}; }

bool startsWith(const std::string& name, const char* prefix) { return name.rfind(prefix, 0) == 0; }

bool allUnit(const common::LargeArray<float>& values) {
    return std::all_of(values.begin(), values.end(), [](float v) { return v >= 0.0F && v <= 1.0F; });
}

// The largest value of an integer component type, as normalized values scale it.
double typeMax(int type) {
    switch (type) {
    case byte_type:
        return 127;
    case unsigned_byte_type:
        return 255;
    case short_type:
        return 32767;
    case unsigned_short_type:
        return 65535;
    default:
        return 4294967295.0;
    }
}

template <typename T>
void store(std::uint8_t* out, double value) {
    const auto clamped = std::clamp(value, static_cast<double>(std::numeric_limits<T>::lowest()),
                                    static_cast<double>(std::numeric_limits<T>::max()));
    const T stored = static_cast<T>(clamped);
    std::memcpy(out, &stored, sizeof(T));
}

void storeComponent(std::uint8_t* out, int type, double value) {
    switch (type) {
    case byte_type:
        store<std::int8_t>(out, value);
        break;
    case unsigned_byte_type:
        store<std::uint8_t>(out, value);
        break;
    case short_type:
        store<std::int16_t>(out, value);
        break;
    case unsigned_short_type:
        store<std::uint16_t>(out, value);
        break;
    case unsigned_int_type:
        store<std::uint32_t>(out, value);
        break;
    default: {
        const auto f = static_cast<float>(value);
        std::memcpy(out, &f, sizeof(float));
    }
    }
}

// Encodes `raw` (count elements of `stride` bytes) with the vertex codec.
EncodedView encodeVertices(const common::LargeArray<std::uint8_t>& raw, std::size_t count, std::size_t stride,
                           const char* filter, common::Workspace& workspace) {
    EncodedView encoded;
    const std::size_t bound = meshopt_encodeVertexBufferBound(count, stride);
    encoded.bytes = common::LargeArray<std::uint8_t>(workspace, bound);
    encoded.size = meshopt_encodeVertexBufferLevel(encoded.bytes.data(), bound, raw.data(), count, stride, 2,
                                                   vertex_codec_version);
    Json extension{{"byteLength", encoded.size}, {"byteStride", stride}, {"count", count}, {"mode", "ATTRIBUTES"}};
    if (filter != nullptr) {
        extension["filter"] = filter;
    }
    Json extensions = Json::object();
    extensions[extension_name] = std::move(extension);
    encoded.view = Json{{"byteLength", count * stride}, {"byteStride", stride}, {"target", array_buffer}};
    encoded.view["extensions"] = std::move(extensions);
    return encoded;
}

void addBounds(Json& accessor, const std::array<double, 3>& min, const std::array<double, 3>& max) {
    accessor["min"] = Json::array({min[0], min[1], min[2]});
    accessor["max"] = Json::array({max[0], max[1], max[2]});
}

} // namespace

PositionGrid gridFor(const std::array<double, 3>& min, const std::array<double, 3>& max) {
    PositionGrid grid;
    double extent = 0;
    for (std::size_t k = 0; k < 3; ++k) {
        grid.center[k] = (min[k] + max[k]) / 2;
        extent = std::max(extent, max[k] - min[k]);
    }
    const double steps = static_cast<double>((1 << (position_bits - 1)) - 1);
    grid.step = extent > 0 ? extent / 2 / steps : 1;
    return grid;
}

SourceType sourceType(const cgltf_accessor& accessor) {
    SourceType type;
    switch (accessor.component_type) {
    case cgltf_component_type_r_8:
        type.component_type = byte_type;
        break;
    case cgltf_component_type_r_8u:
        type.component_type = unsigned_byte_type;
        break;
    case cgltf_component_type_r_16:
        type.component_type = short_type;
        break;
    case cgltf_component_type_r_16u:
        type.component_type = unsigned_short_type;
        break;
    case cgltf_component_type_r_32u:
        type.component_type = unsigned_int_type;
        break;
    default:
        type.component_type = float_type;
    }
    type.normalized = accessor.normalized != 0;
    return type;
}

EncodedView encodeAttribute(const gltf::AttributeData& attribute, const SourceType& source, bool morph_target,
                            const std::optional<PositionGrid>& grid, common::Workspace& workspace) {
    const std::string& name = attribute.name;
    const std::size_t components = attribute.components;
    const std::size_t count = components == 0 ? 0 : attribute.values.size() / components;
    const common::LargeArray<float>& values = attribute.values;
    const bool float_source = source.component_type == float_type;

    // Positions on the mesh's grid, as signed shorts (KHR_mesh_quantization).
    if (!morph_target && name == "POSITION" && grid) {
        const std::size_t stride = 8;
        common::LargeArray<std::uint8_t> raw(workspace, count * stride);
        std::array<double, 3> min{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                                  std::numeric_limits<double>::max()};
        std::array<double, 3> max{-min[0], -min[1], -min[2]};
        for (std::size_t v = 0; v < count; ++v) {
            for (std::size_t k = 0; k < 3; ++k) {
                const double q = std::round((values[3 * v + k] - grid->center[k]) / grid->step);
                storeComponent(&raw[v * stride + 2 * k], short_type, q);
                min[k] = std::min(min[k], q);
                max[k] = std::max(max[k], q);
            }
        }
        EncodedView encoded = encodeVertices(raw, count, stride, nullptr, workspace);
        encoded.accessor = Json{{"componentType", short_type}, {"count", count}, {"type", "VEC3"}};
        addBounds(encoded.accessor, min, max);
        return encoded;
    }

    // Normals and tangents: octahedral, 8 bits (meshopt's OCTAHEDRAL filter).
    if (!morph_target && float_source && (name == "NORMAL" || name == "TANGENT")) {
        const std::size_t stride = 4;
        common::LargeArray<float> vectors(workspace, count * 4);
        for (std::size_t v = 0; v < count; ++v) {
            float x = values[components * v];
            float y = values[components * v + 1];
            float z = values[components * v + 2];
            const float length = std::sqrt(x * x + y * y + z * z);
            if (!(length > 0) || !std::isfinite(length)) {
                x = 0;
                y = 0;
                z = 1;
            }
            vectors[4 * v] = x;
            vectors[4 * v + 1] = y;
            vectors[4 * v + 2] = z;
            vectors[4 * v + 3] = components == 4 ? (values[4 * v + 3] < 0 ? -1.0F : 1.0F) : 0.0F;
        }
        common::LargeArray<std::uint8_t> raw(workspace, count * stride);
        meshopt_encodeFilterOct(raw.data(), count, stride, normal_bits, vectors.data());
        EncodedView encoded = encodeVertices(raw, count, stride, "OCTAHEDRAL", workspace);
        encoded.accessor = Json{{"componentType", byte_type}, {"normalized", true}, {"count", count},
                                {"type", name == "NORMAL" ? "VEC3" : "VEC4"}};
        return encoded;
    }

    // Integer storage for values in [0, 1]: texture coordinates on a 12-bit
    // grid in unsigned shorts, colors and weights in unsigned bytes.
    int type = source.component_type;
    bool normalized = source.normalized;
    double scale = 0;
    double grid_steps = 0;
    bool weights = false;
    if (!morph_target && float_source) {
        if (startsWith(name, "TEXCOORD_") && allUnit(values)) {
            type = unsigned_short_type;
            grid_steps = (1 << texcoord_bits) - 1;
        } else if (startsWith(name, "COLOR_") && allUnit(values)) {
            type = unsigned_byte_type;
            grid_steps = (1 << color_bits) - 1;
        } else if (startsWith(name, "WEIGHTS_") && allUnit(values)) {
            type = unsigned_byte_type;
            grid_steps = (1 << weight_bits) - 1;
            weights = true;
        }
        normalized = type != float_type;
    }
    if (type != float_type) {
        scale = normalized ? typeMax(type) : 1;
    }
    const std::size_t size = componentSize(type);
    const std::size_t stride = align4(size * components);
    common::LargeArray<std::uint8_t> raw(workspace, count * stride);
    std::array<double, 3> min{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                              std::numeric_limits<double>::max()};
    std::array<double, 3> max{-min[0], -min[1], -min[2]};
    for (std::size_t v = 0; v < count; ++v) {
        std::array<double, 4> q{};
        for (std::size_t k = 0; k < components; ++k) {
            const double value = values[components * v + k];
            if (type == float_type) {
                q[k] = value;
            } else if (grid_steps > 0) {
                // On a grid of `grid_steps`, stored at the type's full range.
                q[k] = std::round(std::round(value * grid_steps) * scale / grid_steps);
            } else {
                q[k] = std::round(value * scale);
            }
        }
        if (weights) {
            // Rounding doesn't change a vertex's total weight (one, in valid
            // files): the difference goes to the largest weight.
            double sum = 0;
            double target = 0;
            std::size_t largest = 0;
            for (std::size_t k = 0; k < components; ++k) {
                sum += q[k];
                target += values[components * v + k];
                largest = q[k] > q[largest] ? k : largest;
            }
            q[largest] = std::max(0.0, q[largest] + std::round(target * scale) - sum);
        }
        for (std::size_t k = 0; k < components; ++k) {
            storeComponent(&raw[v * stride + k * size], type, q[k]);
            if (k < 3) {
                min[k] = std::min(min[k], static_cast<double>(static_cast<float>(q[k])));
                max[k] = std::max(max[k], static_cast<double>(static_cast<float>(q[k])));
            }
        }
    }
    EncodedView encoded = encodeVertices(raw, count, stride, nullptr, workspace);
    encoded.accessor = Json{{"componentType", type}, {"count", count}, {"type", typeName(components)}};
    if (normalized) {
        encoded.accessor["normalized"] = true;
    }
    if (name == "POSITION" && components == 3) {
        addBounds(encoded.accessor, min, max);
    }
    return encoded;
}

EncodedView encodeIndices(const common::LargeArray<std::uint32_t>& indices, std::size_t vertex_count, bool triangles,
                          common::Workspace& workspace) {
    EncodedView encoded;
    const std::size_t count = indices.size();
    const bool short_indices = vertex_count <= short_index_vertices;
    const std::size_t stride = short_indices ? 2 : 4;
    const bool as_triangles = triangles && count % 3 == 0;
    const std::size_t bound = as_triangles ? meshopt_encodeIndexBufferBound(count, vertex_count)
                                           : meshopt_encodeIndexSequenceBound(count, vertex_count);
    encoded.bytes = common::LargeArray<std::uint8_t>(workspace, bound);
    encoded.size = as_triangles ? meshopt_encodeIndexBuffer(encoded.bytes.data(), bound, indices.data(), count)
                                : meshopt_encodeIndexSequence(encoded.bytes.data(), bound, indices.data(), count);
    Json extensions = Json::object();
    extensions[extension_name] = Json{{"byteLength", encoded.size},
                                      {"byteStride", stride},
                                      {"count", count},
                                      {"mode", as_triangles ? "TRIANGLES" : "INDICES"}};
    encoded.view = Json{{"byteLength", count * stride}, {"target", element_array_buffer}};
    encoded.view["extensions"] = std::move(extensions);
    encoded.accessor = Json{{"componentType", short_indices ? unsigned_short_type : unsigned_int_type},
                            {"count", count},
                            {"type", "SCALAR"}};
    return encoded;
}

} // namespace meshtinker::compress
