#include "compress.hpp"

#include "encode.hpp"

#include "meshtinker/gltf/edit.hpp"
#include "meshtinker/gltf/geometry.hpp"
#include "meshtinker/gltf/layout.hpp"
#include "meshtinker/gltf/meshopt_memory.hpp"
#include "meshtinker/gltf/primitive_data.hpp"

#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <set>

namespace meshtinker::compress {
namespace {

constexpr std::size_t none = std::numeric_limits<std::size_t>::max();

enum class Plan : std::uint8_t { rewrite, draco, meshopt, unreadable };

std::size_t indexOf(const cgltf_accessor* accessor, const cgltf_data& data) {
    return static_cast<std::size_t>(accessor - data.accessors);
}

bool isTriangles(cgltf_primitive_type type) {
    return type == cgltf_primitive_type_triangles || type == cgltf_primitive_type_triangle_strip ||
           type == cgltf_primitive_type_triangle_fan;
}

const cgltf_accessor* attributeNamed(const cgltf_attribute* attributes, std::size_t count, const std::string& name) {
    for (std::size_t a = 0; a < count; ++a) {
        if (attributes[a].name != nullptr && name == attributes[a].name) {
            return attributes[a].data;
        }
    }
    return nullptr;
}

std::array<double, 3> rotate(const std::array<double, 4>& q, const std::array<double, 3>& v) {
    // v + 2w(u × v) + 2u × (u × v), with u the quaternion's vector part.
    const std::array<double, 3> u{q[0], q[1], q[2]};
    const auto cross = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
        return std::array<double, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    std::array<double, 3> t = cross(u, v);
    for (double& x : t) {
        x *= 2;
    }
    const std::array<double, 3> ut = cross(u, t);
    return {v[0] + q[3] * t[0] + ut[0], v[1] + q[3] * t[1] + ut[1], v[2] + q[3] * t[2] + ut[2]};
}

template <std::size_t N>
std::array<double, N> numbers(const Json& node, const char* key, const std::array<double, N>& fallback) {
    std::array<double, N> values = fallback;
    const auto found = node.find(key);
    if (found != node.end() && found->is_array() && found->size() == N) {
        for (std::size_t i = 0; i < N; ++i) {
            values[i] = (*found)[i].get<double>();
        }
    }
    return values;
}

// Folds the grid's dequantization, translate(center) × scale(nodeScale), into
// the node's own transform, on the right: node × grid.
void composeGrid(Json& node, const PositionGrid& grid) {
    const double k = grid.nodeScale();
    const std::array<double, 3>& c = grid.center;
    if (node.contains("matrix")) {
        std::array<double, 16> m = numbers<16>(node, "matrix", {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
        std::array<double, 16> out = m;
        for (std::size_t i = 0; i < 3; ++i) {
            out[12 + i] = m[i] * c[0] + m[4 + i] * c[1] + m[8 + i] * c[2] + m[12 + i];
        }
        for (std::size_t i = 0; i < 12; ++i) {
            if (i % 4 != 3) {
                out[i] = m[i] * k;
            }
        }
        node["matrix"] = out;
        return;
    }
    const std::array<double, 3> t = numbers<3>(node, "translation", {0, 0, 0});
    const std::array<double, 4> r = numbers<4>(node, "rotation", {0, 0, 0, 1});
    const std::array<double, 3> s = numbers<3>(node, "scale", {1, 1, 1});
    const std::array<double, 3> moved = rotate(r, {s[0] * c[0], s[1] * c[1], s[2] * c[2]});
    node["translation"] = std::array<double, 3>{t[0] + moved[0], t[1] + moved[1], t[2] + moved[2]};
    node["scale"] = std::array<double, 3>{s[0] * k, s[1] * k, s[2] * k};
}

std::string primitiveName(std::size_t mesh, std::size_t primitive) {
    return "mesh " + std::to_string(mesh) + " primitive " + std::to_string(primitive);
}

} // namespace

Report compress(const std::filesystem::path& input, const std::filesystem::path& directory, const std::string& stem,
                const Options& options, common::Workspace& workspace, common::Outputs& outputs,
                common::Diagnostics& diagnostics) {
    gltf::Document document = gltf::readNormalized(input, options.read, diagnostics);
    gltf::Layout layout = gltf::normalizedLayout(document, gltf::selectScene(document), diagnostics);
    gltf::GeometryReader reader(document, workspace);
    const gltf::MeshoptMemory meshopt_memory(workspace);
    const cgltf_data& data = document.data();
    Report report;

    // What happens to each primitive.
    std::vector<std::vector<Plan>> plans(data.meshes_count);
    for (std::size_t m = 0; m < data.meshes_count; ++m) {
        for (std::size_t p = 0; p < data.meshes[m].primitives_count; ++p) {
            const cgltf_primitive& primitive = data.meshes[m].primitives[p];
            Plan plan = Plan::rewrite;
            if (gltf::GeometryReader::isDraco(primitive)) {
                plan = Plan::draco;
            } else {
                gltf::forEachAccessor(primitive, [&](const cgltf_accessor& accessor) {
                    if (gltf::usesMeshopt(accessor)) {
                        plan = Plan::meshopt;
                    }
                });
            }
            if (plan == Plan::rewrite) {
                gltf::forEachAccessor(primitive, [&](const cgltf_accessor& accessor) {
                    if (reader.prepare(accessor) != gltf::Readability::readable ||
                        cgltf_num_components(accessor.type) > 4) {
                        plan = Plan::unreadable;
                    }
                });
                if (gltf::findAttribute(primitive, cgltf_attribute_type_position, 0) == nullptr) {
                    plan = Plan::unreadable;
                }
            }
            switch (plan) {
            case Plan::draco:
                ++report.kept_draco;
                diagnostics.warn("draco-kept", "Draco primitives were copied as they are", primitiveName(m, p));
                break;
            case Plan::meshopt:
                ++report.kept_meshopt;
                break;
            case Plan::unreadable:
                ++report.kept_unreadable;
                diagnostics.warn("primitive-not-compressed",
                                 "Primitives whose geometry can't be read were copied as they are",
                                 primitiveName(m, p));
                break;
            case Plan::rewrite:
                break;
            }
            plans[m].push_back(plan);
        }
    }

    // How many primitives use each accessor, and which mesh each POSITION is in.
    std::vector<std::uint32_t> users(data.accessors_count, 0);
    std::vector<std::size_t> position_mesh(data.accessors_count, none);
    std::vector<bool> position_shared(data.accessors_count, false);
    for (std::size_t m = 0; m < data.meshes_count; ++m) {
        for (std::size_t p = 0; p < data.meshes[m].primitives_count; ++p) {
            const cgltf_primitive& primitive = data.meshes[m].primitives[p];
            gltf::forEachAccessor(primitive, [&](const cgltf_accessor& accessor) { ++users[indexOf(&accessor, data)]; });
            const cgltf_accessor* position = gltf::findAttribute(primitive, cgltf_attribute_type_position, 0);
            if (position != nullptr) {
                const std::size_t index = indexOf(position, data);
                if (position_mesh[index] != none && position_mesh[index] != m) {
                    position_shared[index] = true;
                }
                position_mesh[index] = m;
            }
        }
    }
    // Meshes whose node transform can't carry a grid: skinned ones ignore it,
    // and instancing applies its transforms before the node's.
    std::vector<bool> grid_forbidden(data.meshes_count, false);
    for (std::size_t n = 0; n < data.nodes_count; ++n) {
        const cgltf_node& node = data.nodes[n];
        if (node.mesh != nullptr && (node.skin != nullptr || node.has_mesh_gpu_instancing != 0)) {
            grid_forbidden[static_cast<std::size_t>(node.mesh - data.meshes)] = true;
        }
    }
    // Nodes whose transform an animation sets: their own transform can't
    // take the grid, which goes on a new child node instead.
    std::set<std::size_t> animated;
    for (std::size_t a = 0; a < data.animations_count; ++a) {
        for (std::size_t c = 0; c < data.animations[a].channels_count; ++c) {
            const cgltf_animation_channel& channel = data.animations[a].channels[c];
            if (channel.target_node != nullptr && channel.target_path != cgltf_animation_path_type_weights) {
                animated.insert(static_cast<std::size_t>(channel.target_node - data.nodes));
            }
        }
    }

    std::deque<EncodedView> encoded;
    std::set<const cgltf_buffer_view*> input_views;
    const auto add = [&](EncodedView&& view, std::size_t source) {
        report.output_bytes += view.size;
        encoded.push_back(std::move(view));
        EncodedView& kept = encoded.back();
        const std::size_t index = gltf::addView(layout, kept.view, {kept.bytes.data(), kept.size});
        kept.accessor["bufferView"] = index;
        gltf::copyAccessorProperties(layout, source, kept.accessor);
        return gltf::addAccessor(layout, kept.accessor);
    };
    // Accessors several primitives share are encoded once: by source
    // accessor, role (0 vertex, 1 morph target, 2 indices) and grid's mesh.
    std::map<std::array<std::size_t, 3>, std::size_t> shared_encoded;

    // A mesh can take a grid when every primitive is rewritten, without
    // morph targets, from float positions it doesn't share with other
    // meshes. They all share one grid over their positions as stored, as
    // gltfpack's default and glTF-Transform's quantizationVolume "scene" do:
    // the borders between tiles then quantize the same on both sides, however
    // the tiles are split into meshes.
    std::vector<bool> eligible(data.meshes_count, false);
    for (std::size_t m = 0; m < data.meshes_count; ++m) {
        const cgltf_mesh& mesh = data.meshes[m];
        bool can = !grid_forbidden[m];
        for (std::size_t p = 0; p < mesh.primitives_count; ++p) {
            const cgltf_primitive& primitive = mesh.primitives[p];
            const cgltf_accessor* position = gltf::findAttribute(primitive, cgltf_attribute_type_position, 0);
            can = can && plans[m][p] == Plan::rewrite && primitive.targets_count == 0 && position != nullptr &&
                  position->component_type == cgltf_component_type_r_32f && !position_shared[indexOf(position, data)];
        }
        eligible[m] = can;
    }
    std::optional<PositionGrid> scene_grid;
    {
        std::array<double, 3> min{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                                  std::numeric_limits<double>::max()};
        std::array<double, 3> max{-min[0], -min[1], -min[2]};
        for (std::size_t m = 0; m < data.meshes_count; ++m) {
            if (!eligible[m]) {
                continue;
            }
            for (std::size_t p = 0; p < data.meshes[m].primitives_count; ++p) {
                gltf::AttributeData position;
                if (!gltf::readAccessor(data, *gltf::findAttribute(data.meshes[m].primitives[p], cgltf_attribute_type_position, 0),
                                        "POSITION", workspace, position)) {
                    continue;
                }
                for (std::size_t i = 0; i + 2 < position.values.size(); i += 3) {
                    for (std::size_t k = 0; k < 3; ++k) {
                        min[k] = std::min(min[k], static_cast<double>(position.values[i + k]));
                        max[k] = std::max(max[k], static_cast<double>(position.values[i + k]));
                    }
                }
            }
        }
        if (min[0] <= max[0]) {
            scene_grid = gridFor(min, max);
        }
    }

    for (std::size_t m = 0; m < data.meshes_count; ++m) {
        const cgltf_mesh& mesh = data.meshes[m];
        bool rewrites = false;
        for (std::size_t p = 0; p < mesh.primitives_count; ++p) {
            rewrites = rewrites || plans[m][p] == Plan::rewrite;
        }
        if (!rewrites) {
            continue;
        }

        // Primitives that own all their accessors are reordered for the
        // vertex cache, then for vertex fetch, as glTF-Transform's
        // reorder({target: "size"}) does before meshopt compression.
        std::vector<std::optional<gltf::TriangleData>> owned(mesh.primitives_count);
        for (std::size_t p = 0; p < mesh.primitives_count; ++p) {
            if (plans[m][p] != Plan::rewrite) {
                continue;
            }
            const cgltf_primitive& primitive = mesh.primitives[p];
            gltf::forEachAccessor(primitive, [&](const cgltf_accessor& accessor) {
                input_views.insert(accessor.buffer_view);
                if (accessor.is_sparse != 0) {
                    input_views.insert(accessor.sparse.indices_buffer_view);
                    input_views.insert(accessor.sparse.values_buffer_view);
                }
            });
            bool exclusive = isTriangles(primitive.type);
            gltf::forEachAccessor(primitive, [&](const cgltf_accessor& accessor) {
                exclusive = exclusive && users[indexOf(&accessor, data)] == 1;
            });
            if (!exclusive) {
                continue;
            }
            gltf::TriangleData triangles;
            if (gltf::readTriangles(document, reader, workspace, m, p, triangles) != gltf::TriangleReadStatus::ok ||
                triangles.indices.empty()) {
                continue;
            }
            meshopt_optimizeVertexCacheStrip(triangles.indices.data(), triangles.indices.data(),
                                             triangles.indices.size(), triangles.vertex_count);
            common::LargeArray<std::uint32_t> indices = std::move(triangles.indices);
            owned[p] = gltf::compactVertices(triangles, std::move(indices), workspace);
        }

        const std::optional<PositionGrid> grid = eligible[m] ? scene_grid : std::nullopt;
        if (grid) {
            ++report.quantized_meshes;
        } else {
            ++report.float_meshes;
        }

        for (std::size_t p = 0; p < mesh.primitives_count; ++p) {
            if (plans[m][p] != Plan::rewrite) {
                continue;
            }
            const cgltf_primitive& primitive = mesh.primitives[p];
            gltf::PrimitiveGeometry geometry;
            std::optional<gltf::TriangleData>& entry = owned[p];
            if (entry) {
                gltf::TriangleData& triangles = *entry;
                for (const gltf::AttributeData& attribute : triangles.attributes) {
                    const cgltf_accessor* source =
                        attributeNamed(primitive.attributes, primitive.attributes_count, attribute.name);
                    geometry.attributes[attribute.name] = add(
                        encodeAttribute(attribute, sourceType(*source), false, grid, workspace), attribute.source_accessor);
                }
                if (!triangles.targets.empty()) {
                    geometry.targets = Json::array();
                    for (std::size_t t = 0; t < triangles.targets.size(); ++t) {
                        Json target = Json::object();
                        for (const gltf::AttributeData& attribute : triangles.targets[t]) {
                            const cgltf_accessor* source = attributeNamed(
                                primitive.targets[t].attributes, primitive.targets[t].attributes_count, attribute.name);
                            target[attribute.name] =
                                add(encodeAttribute(attribute, sourceType(*source), true, grid, workspace),
                                    attribute.source_accessor);
                        }
                        geometry.targets.push_back(std::move(target));
                    }
                }
                geometry.indices = add(encodeIndices(triangles.indices, triangles.vertex_count, true, workspace),
                                       triangles.indices_source);
                geometry.mode = 4;
                entry.reset();
            } else {
                const auto encodeShared = [&](const cgltf_attribute& attribute, std::size_t role) {
                    const std::size_t index = indexOf(attribute.data, data);
                    const bool on_grid = grid && role == 0 && std::string(attribute.name) == "POSITION";
                    const std::array<std::size_t, 3> key{index, role, on_grid ? m : none};
                    if (const auto found = shared_encoded.find(key); found != shared_encoded.end()) {
                        return found->second;
                    }
                    gltf::AttributeData values;
                    gltf::readAccessor(data, *attribute.data, attribute.name, workspace, values);
                    const std::size_t written = add(encodeAttribute(values, sourceType(*attribute.data), role == 1,
                                                                    on_grid ? grid : std::nullopt, workspace),
                                                    index);
                    shared_encoded[key] = written;
                    return written;
                };
                for (std::size_t a = 0; a < primitive.attributes_count; ++a) {
                    geometry.attributes[primitive.attributes[a].name] = encodeShared(primitive.attributes[a], 0);
                }
                if (primitive.targets_count > 0) {
                    geometry.targets = Json::array();
                    for (std::size_t t = 0; t < primitive.targets_count; ++t) {
                        Json target = Json::object();
                        for (std::size_t a = 0; a < primitive.targets[t].attributes_count; ++a) {
                            target[primitive.targets[t].attributes[a].name] =
                                encodeShared(primitive.targets[t].attributes[a], 1);
                        }
                        geometry.targets.push_back(std::move(target));
                    }
                }
                if (primitive.indices != nullptr) {
                    const std::size_t index = indexOf(primitive.indices, data);
                    const std::array<std::size_t, 3> key{index, 2, none};
                    if (const auto found = shared_encoded.find(key); found != shared_encoded.end()) {
                        geometry.indices = found->second;
                    } else {
                        common::LargeArray<std::uint32_t> indices(workspace, primitive.indices->count);
                        if (cgltf_accessor_unpack_indices(primitive.indices, indices.data(), sizeof(std::uint32_t),
                                                          indices.size()) != indices.size()) {
                            for (std::size_t i = 0; i < indices.size(); ++i) {
                                indices[i] = static_cast<std::uint32_t>(cgltf_accessor_read_index(primitive.indices, i));
                            }
                        }
                        const std::size_t vertices =
                            gltf::findAttribute(primitive, cgltf_attribute_type_position, 0)->count;
                        geometry.indices =
                            add(encodeIndices(indices, vertices, primitive.type == cgltf_primitive_type_triangles,
                                              workspace),
                                index);
                        shared_encoded[key] = *geometry.indices;
                    }
                }
                geometry.mode = layout.json["meshes"][m]["primitives"][p].value("mode", 4);
            }
            gltf::replaceGeometry(layout, m, p, geometry);
            ++report.compressed;
        }

        if (grid) {
            Json& nodes = layout.json["nodes"];
            const std::size_t node_count = nodes.size();
            for (std::size_t n = 0; n < node_count; ++n) {
                Json& node = nodes[n];
                if (!(node.contains("mesh") && node["mesh"].get<std::size_t>() == m)) {
                    continue;
                }
                const bool carries_others = node.contains("children") || node.contains("camera") ||
                                            animated.count(n) != 0;
                if (!carries_others) {
                    composeGrid(node, *grid);
                    continue;
                }
                // The mesh moves to a new child that holds only the grid. It
                // comes first among the children, so that traversal still
                // reaches the mesh before what the node's children draw.
                Json child{{"mesh", m}, {"translation", grid->center},
                           {"scale", std::array<double, 3>{grid->nodeScale(), grid->nodeScale(), grid->nodeScale()}}};
                node.erase("mesh");
                nodes.push_back(std::move(child));
                Json children = Json::array({nodes.size() - 1});
                if (nodes[n].contains("children")) {
                    for (const Json& existing : nodes[n]["children"]) {
                        children.push_back(existing);
                    }
                }
                nodes[n]["children"] = std::move(children);
                ++report.added_nodes;
            }
        }
    }

    if (report.compressed > 0) {
        gltf::useExtension(layout.json, "EXT_meshopt_compression", true);
    }
    if (gltf::needsMeshQuantization(layout.json)) {
        gltf::useExtension(layout.json, "KHR_mesh_quantization", true);
    }
    gltf::prune(layout);
    for (const cgltf_buffer_view* view : input_views) {
        if (view != nullptr) {
            report.input_bytes += view->has_meshopt_compression != 0 ? view->meshopt_compression.size : view->size;
        }
    }
    report.files = gltf::writeSeparate(document, layout, directory, stem, outputs);
    return report;
}

} // namespace meshtinker::compress
