#include "model.hpp"

#include "meshtinker/common/failure.hpp"

#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <utility>

namespace meshtinker::placeholder {
namespace {

using gltf::Json;
using gltf::Matrix;

constexpr double infinity = std::numeric_limits<double>::infinity();

std::string primitiveName(const Json& json, std::size_t mesh, std::size_t primitive) {
    const Json& object = json["meshes"][mesh];
    const auto name = object.find("name");
    const bool named = name != object.end() && name->is_string() && !name->get<std::string>().empty();
    return (named ? "mesh \"" + name->get<std::string>() + "\"" : "mesh " + std::to_string(mesh)) + ", primitive " +
           std::to_string(primitive);
}

void warnUnreadable(common::Diagnostics& diagnostics, gltf::Readability status, const std::string& where) {
    switch (status) {
    case gltf::Readability::readable:
        break;
    case gltf::Readability::draco_failed:
        diagnostics.warn("draco-decode-failed",
                         "Draco-compressed data could not be decoded, or doesn't match its accessors: it is left out "
                         "of the placeholder",
                         where);
        break;
    case gltf::Readability::no_data:
        diagnostics.warn("missing-vertex-data", "Vertex data is missing: it is left out of the placeholder", where);
        break;
    case gltf::Readability::meshopt_failed:
        diagnostics.warn("meshopt-decode-failed",
                         "meshopt-compressed data could not be decoded: it is left out of the placeholder", where);
        break;
    }
}

// Where an accessor's elements start, for direct reads, or null.
const std::uint8_t* directData(const cgltf_accessor& accessor) {
    if (accessor.is_sparse || accessor.buffer_view == nullptr) {
        return nullptr;
    }
    const std::uint8_t* view = cgltf_buffer_view_data(accessor.buffer_view);
    return view == nullptr ? nullptr : view + accessor.offset;
}

std::optional<Primitive> readPrimitive(const gltf::Document& document, gltf::GeometryReader& reader,
                                       std::size_t mesh, std::size_t index, common::Diagnostics& diagnostics) {
    const cgltf_primitive& source = document.data().meshes[mesh].primitives[index];
    const std::string where = primitiveName(document.json(), mesh, index);
    if (source.type != cgltf_primitive_type_triangles && source.type != cgltf_primitive_type_triangle_strip &&
        source.type != cgltf_primitive_type_triangle_fan) {
        diagnostics.warn("points-and-lines-left-out",
                         "Points and lines have no surface: they are left out of the placeholder", where);
        return std::nullopt;
    }
    const cgltf_accessor* position = gltf::findAttribute(source, cgltf_attribute_type_position, 0);
    if (position == nullptr) {
        return std::nullopt;
    }
    gltf::Readability status = reader.preparePrimitive(mesh, index);
    if (status == gltf::Readability::readable) {
        status = reader.prepare(*position);
    }
    if (status == gltf::Readability::readable && source.indices != nullptr) {
        status = reader.prepare(*source.indices);
    }
    if (status != gltf::Readability::readable) {
        warnUnreadable(diagnostics, status, where);
        return std::nullopt;
    }

    Primitive primitive;
    primitive.position = position;
    primitive.indices = source.indices;
    primitive.type = source.type;
    primitive.triangles = gltf::triangleCount(source.type, gltf::cornerCount(source));
    if (position->component_type == cgltf_component_type_r_32f && position->type == cgltf_type_vec3 &&
        !position->normalized) {
        primitive.position_data = directData(*position);
        primitive.position_stride = position->stride;
    }
    if (source.indices != nullptr && (source.indices->component_type == cgltf_component_type_r_16u ||
                                      source.indices->component_type == cgltf_component_type_r_32u)) {
        primitive.index_data = directData(*source.indices);
        if (primitive.index_data != nullptr) {
            primitive.index_stride = source.indices->stride;
            primitive.index_size = source.indices->component_type == cgltf_component_type_r_32u ? 4 : 2;
        }
    }
    // Every vertex, not only those the triangles use: one pass over the
    // positions, which accessor min and max can't replace since they may be
    // missing or wrong.
    primitive.min = {infinity, infinity, infinity};
    primitive.max = {-infinity, -infinity, -infinity};
    for (std::uint64_t v = 0; v < position->count; ++v) {
        const Vec3 p = primitive.point(v);
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) {
            continue;
        }
        for (std::size_t k = 0; k < 3; ++k) {
            primitive.min[k] = std::min(primitive.min[k], p[k]);
            primitive.max[k] = std::max(primitive.max[k], p[k]);
        }
    }
    return primitive;
}

Matrix worldMatrix(const cgltf_node& node) {
    Matrix world = gltf::localMatrix(node);
    for (const cgltf_node* parent = node.parent; parent != nullptr; parent = parent->parent) {
        world = gltf::multiply(gltf::localMatrix(*parent), world);
    }
    return world;
}

// Where a skinned mesh is drawn at rest. The spec ignores a skinned mesh's
// node: a renderer places each vertex by its joints' world matrices times
// their inverse bind matrices. In a model's rest pose that product is the
// same for every joint, so the first joint's places the whole mesh, without
// reading joints and weights per vertex.
Matrix skinnedMatrix(const cgltf_skin& skin, gltf::GeometryReader& reader, common::Diagnostics& diagnostics,
                     const std::string& where) {
    if (skin.joints_count == 0) {
        return gltf::identity;
    }
    Matrix inverse_bind = gltf::identity;
    if (skin.inverse_bind_matrices != nullptr) {
        const gltf::Readability status = reader.prepare(*skin.inverse_bind_matrices);
        float values[16] = {};
        if (status != gltf::Readability::readable ||
            !cgltf_accessor_read_float(skin.inverse_bind_matrices, 0, values, 16)) {
            warnUnreadable(diagnostics, status, where);
        } else {
            for (std::size_t i = 0; i < 16; ++i) {
                inverse_bind[i] = values[i];
            }
        }
    }
    return gltf::multiply(worldMatrix(*skin.joints[0]), inverse_bind);
}

// A node's transform properties, as written, or nothing if it has none.
std::optional<Json> transformOf(const Json& node) {
    Json kept = Json::object();
    for (const char* key : {"matrix", "translation", "rotation", "scale"}) {
        const auto found = node.find(key);
        if (found != node.end()) {
            kept[key] = *found;
        }
    }
    return kept.empty() ? std::nullopt : std::optional<Json>(std::move(kept));
}

} // namespace

Model Model::read(gltf::Document& document, gltf::GeometryReader& reader, common::Diagnostics& diagnostics) {
    const cgltf_data& data = document.data();
    const Json& json = document.json();

    // The scene three.js shows; without scenes, every root node, as the
    // profile keeps everything then.
    std::vector<const cgltf_node*> roots;
    if (data.scenes_count > 0) {
        const cgltf_scene& scene = data.scene != nullptr ? *data.scene : data.scenes[0];
        roots.assign(scene.nodes, scene.nodes + scene.nodes_count);
    } else {
        for (std::size_t n = 0; n < data.nodes_count; ++n) {
            if (data.nodes[n].parent == nullptr) {
                roots.push_back(&data.nodes[n]);
            }
        }
    }

    struct Placed {
        std::size_t first;
        std::size_t last;
        Matrix world;
        std::optional<gltf::Instancing> instancing;
        std::uint64_t instances;
    };
    Model model;
    std::map<std::size_t, std::pair<std::size_t, std::size_t>> of_mesh;
    std::vector<Placed> placed;
    std::optional<Matrix> frame_world;
    Matrix frame_inverse = gltf::identity;
    const cgltf_node* frame_node = nullptr;

    // Depth first in file order, so that which node gives the frame doesn't
    // depend on the walk. The node tree is a tree (cgltf_validate rejects
    // cycles), but a node listed twice by a scene is visited once.
    std::vector<bool> visited(data.nodes_count, false);
    std::vector<std::pair<const cgltf_node*, Matrix>> stack;
    for (auto root = roots.rbegin(); root != roots.rend(); ++root) {
        stack.emplace_back(*root, gltf::localMatrix(**root));
    }
    while (!stack.empty()) {
        const auto [node, world] = stack.back();
        stack.pop_back();
        const auto id = static_cast<std::size_t>(node - data.nodes);
        if (visited[id]) {
            continue;
        }
        visited[id] = true;
        if (node->mesh != nullptr) {
            const auto mesh = static_cast<std::size_t>(node->mesh - data.meshes);
            if (of_mesh.find(mesh) == of_mesh.end()) {
                const std::size_t first = model.primitives_.size();
                for (std::size_t p = 0; p < node->mesh->primitives_count; ++p) {
                    if (std::optional<Primitive> primitive = readPrimitive(document, reader, mesh, p, diagnostics)) {
                        model.primitives_.push_back(*primitive);
                    }
                }
                of_mesh.emplace(mesh, std::make_pair(first, model.primitives_.size()));
            }
            const bool skinned = node->skin != nullptr;
            const Matrix base =
                skinned ? skinnedMatrix(*node->skin, reader, diagnostics, "node " + std::to_string(id) + " skin") : world;
            // A skinned node is never the frame: no node chain carries its
            // matrix into the placeholder.
            if (!frame_world && !skinned) {
                if (const std::optional<Matrix> inverse = gltf::invertAffine(base)) {
                    frame_world = base;
                    frame_inverse = *inverse;
                    frame_node = node;
                }
            }
            std::optional<gltf::Instancing> instances;
            bool readable = true;
            if (node->has_mesh_gpu_instancing) {
                instances = gltf::instancing(*node);
                for (const cgltf_accessor* accessor : {instances->translation, instances->rotation, instances->scale}) {
                    const gltf::Readability status =
                        accessor == nullptr || !readable ? gltf::Readability::readable : reader.prepare(*accessor);
                    if (status != gltf::Readability::readable) {
                        warnUnreadable(diagnostics, status, "node " + std::to_string(id) + " instances");
                        readable = false;
                    }
                }
            }
            if (readable) {
                const auto [first, last] = of_mesh[mesh];
                placed.push_back({first, last, base, instances, instances ? instances->count : 1});
            }
        }
        for (std::size_t c = node->children_count; c-- > 0;) {
            const cgltf_node* child = node->children[c];
            stack.emplace_back(child, gltf::multiply(world, gltf::localMatrix(*child)));
        }
    }

    // A model flattened by all its transforms, or only skinned, has no frame
    // of its own, and is built in world coordinates.
    const Matrix frame = frame_world.value_or(gltf::identity);
    for (const cgltf_node* node = frame_node; node != nullptr; node = node->parent) {
        if (std::optional<Json> transform = transformOf(json["nodes"][static_cast<std::size_t>(node - data.nodes)])) {
            model.frame_.insert(model.frame_.begin(), std::move(*transform));
        }
    }

    model.min_ = {infinity, infinity, infinity};
    model.max_ = {-infinity, -infinity, -infinity};
    for (const Placed& p : placed) {
        Placement placement;
        placement.first = p.first;
        placement.last = p.last;
        if (p.world != frame) {
            placement.to_frame = gltf::multiply(frame_inverse, p.world);
        }
        placement.instancing = p.instancing;
        placement.instances = p.instances;
        // Each primitive's box through each instance: reading every vertex
        // once per instance wouldn't scale.
        for (std::size_t i = p.first; i < p.last; ++i) {
            const Primitive& primitive = model.primitives_[i];
            model.faces_ += primitive.triangles * p.instances;
            if (primitive.triangles == 0 || primitive.min[0] > primitive.max[0]) {
                continue;
            }
            for (std::uint64_t instance = 0; instance < p.instances; ++instance) {
                const std::optional<Matrix> matrix = placement.matrix(instance);
                for (unsigned corner = 0; corner < 8; ++corner) {
                    Vec3 v{};
                    for (std::size_t k = 0; k < 3; ++k) {
                        v[k] = ((corner >> k) & 1U) == 0 ? primitive.min[k] : primitive.max[k];
                    }
                    if (matrix) {
                        v = gltf::apply(*matrix, v);
                    }
                    for (std::size_t k = 0; k < 3; ++k) {
                        model.min_[k] = std::min(model.min_[k], v[k]);
                        model.max_[k] = std::max(model.max_[k], v[k]);
                    }
                }
            }
        }
        model.placements_.push_back(placement);
    }
    if (model.faces_ == 0) {
        throw common::rejected("no-faces", "the scene shows no triangles to build a placeholder around");
    }
    return model;
}

} // namespace meshtinker::placeholder
