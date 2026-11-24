#include "meshtinker/analysis/analyze.hpp"

#include "meshtinker/analysis/coverage.hpp"
#include "meshtinker/common/failure.hpp"
#include "meshtinker/common/references.hpp"
#include "meshtinker/gltf/geometry.hpp"
#include "meshtinker/gltf/graph.hpp"
#include "meshtinker/gltf/transform.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <tuple>

namespace meshtinker::analysis {
namespace {

using gltf::cornerCount;
using gltf::findAttribute;
using gltf::identity;
using gltf::instanceMatrix;
using gltf::Instancing;
using gltf::instancing;
using gltf::Json;
using gltf::Kind;
using gltf::localMatrix;
using gltf::Matrix;
using gltf::multiply;
using gltf::triangleCorners;
using gltf::triangleCount;

// Extensions whose meaning the figures account for, or that don't change them.
bool isUnderstoodExtension(const std::string& name) {
    return gltf::isIndexAwareExtension(name);
}

// Slots holding the base color: core, and the archived specular-glossiness
// extension's diffuse texture.
bool isDiffuseSlot(const std::string& slot) {
    return slot == "baseColorTexture" || slot == "KHR_materials_pbrSpecularGlossiness.diffuseTexture";
}

std::optional<std::string> nonEmptyName(const Json& object) {
    const auto name = object.find("name");
    if (name != object.end() && name->is_string() && !name->get<std::string>().empty()) {
        return name->get<std::string>();
    }
    return std::nullopt;
}

std::vector<std::string> stringArray(const Json& json, const char* key) {
    std::vector<std::string> values;
    const auto found = json.find(key);
    if (found != json.end() && found->is_array()) {
        for (const Json& value : *found) {
            if (value.is_string()) {
                values.push_back(value.get<std::string>());
            }
        }
    }
    return values;
}

std::string primitiveName(const gltf::Document& document, std::size_t mesh, std::size_t primitive) {
    const Json& json = document.json()["meshes"][mesh];
    const std::optional<std::string> name = nonEmptyName(json);
    return (name ? "mesh \"" + *name + "\"" : "mesh " + std::to_string(mesh)) + ", primitive " + std::to_string(primitive);
}

// Component types KHR_mesh_quantization allows and core glTF doesn't.
bool isQuantized(const cgltf_attribute& attribute) {
    const cgltf_accessor* accessor = attribute.data;
    if (accessor == nullptr || accessor->component_type == cgltf_component_type_r_32f) {
        return false;
    }
    switch (attribute.type) {
    case cgltf_attribute_type_position:
    case cgltf_attribute_type_normal:
    case cgltf_attribute_type_tangent:
        return true;
    case cgltf_attribute_type_texcoord: {
        // Core allows normalized unsigned bytes and shorts only.
        const bool core = accessor->normalized &&
                          (accessor->component_type == cgltf_component_type_r_8u ||
                           accessor->component_type == cgltf_component_type_r_16u);
        return !core;
    }
    default:
        return false;
    }
}

bool usesMeshopt(const cgltf_primitive& primitive) {
    const auto compressed = [](const cgltf_accessor* accessor) {
        return accessor != nullptr && accessor->buffer_view != nullptr && accessor->buffer_view->has_meshopt_compression;
    };
    if (compressed(primitive.indices)) {
        return true;
    }
    for (std::size_t a = 0; a < primitive.attributes_count; ++a) {
        if (compressed(primitive.attributes[a].data)) {
            return true;
        }
    }
    return false;
}

std::size_t indexOf(const cgltf_data& data, const cgltf_mesh* mesh) {
    return static_cast<std::size_t>(mesh - data.meshes);
}

// KHR_texture_transform, the UVs a renderer actually samples with:
// T · R · S · uv, from the extension's reference shader, as objtools applies it.
struct UvTransform {
    std::array<std::array<double, 3>, 2> rows{{{1, 0, 0}, {0, 1, 0}}};
    std::optional<std::uint64_t> tex_coord;

    std::array<double, 2> apply(double u, double v) const {
        return {rows[0][0] * u + rows[0][1] * v + rows[0][2], rows[1][0] * u + rows[1][1] * v + rows[1][2]};
    }
    bool operator<(const UvTransform& other) const {
        return std::tie(rows, tex_coord) < std::tie(other.rows, other.tex_coord);
    }
};

UvTransform uvTransform(const Json& info) {
    UvTransform transform;
    const auto extensions = info.find("extensions");
    if (extensions == info.end() || !extensions->is_object()) {
        return transform;
    }
    const auto found = extensions->find("KHR_texture_transform");
    if (found == extensions->end() || !found->is_object()) {
        return transform;
    }
    const Json& ext = *found;
    const auto pair = [&](const char* key, double fallback) {
        std::array<double, 2> value{fallback, fallback};
        const auto it = ext.find(key);
        if (it != ext.end() && it->is_array() && it->size() == 2 && (*it)[0].is_number() && (*it)[1].is_number()) {
            value = {(*it)[0].get<double>(), (*it)[1].get<double>()};
        }
        return value;
    };
    const auto offset = pair("offset", 0.0);
    const auto scale = pair("scale", 1.0);
    const double rotation = ext.value("rotation", 0.0);
    const double s = std::sin(rotation);
    const double c = std::cos(rotation);
    transform.rows = {{{c * scale[0], -s * scale[1], offset[0]}, {s * scale[0], c * scale[1], offset[1]}}};
    const auto tex_coord = ext.find("texCoord");
    if (tex_coord != ext.end() && tex_coord->is_number_unsigned()) {
        transform.tex_coord = tex_coord->get<std::uint64_t>();
    }
    return transform;
}

struct SamplerSettings {
    std::array<Wrap, 2> wrap{Wrap::repeat, Wrap::repeat};
    Filter filter = Filter::linear;

    bool operator<(const SamplerSettings& other) const {
        return std::tie(wrap, filter) < std::tie(other.wrap, other.filter);
    }
};

// One primitive drawn with one texture: which triangles, which UVs, how sampled.
struct Draw {
    std::size_t mesh = 0;
    std::size_t primitive = 0;
    std::uint64_t set = 0;
    UvTransform transform;
    SamplerSettings sampler;

    bool operator<(const Draw& other) const {
        return std::tie(mesh, primitive, set, transform, sampler) <
               std::tie(other.mesh, other.primitive, other.set, other.transform, other.sampler);
    }
};

SamplerSettings samplerSettings(const Json& json, const Json& texture) {
    SamplerSettings settings;
    const auto sampler = texture.find("sampler");
    if (sampler == texture.end() || !sampler->is_number_unsigned()) {
        return settings;
    }
    const Json& s = json["samplers"][sampler->get<std::size_t>()];
    const auto mode = [&](const char* key) {
        const auto it = s.find(key);
        const std::uint64_t value = it != s.end() && it->is_number_unsigned() ? it->get<std::uint64_t>() : 10497;
        return wrapFromGl(value).value_or(Wrap::repeat);
    };
    const auto filter = [&](const char* key) -> std::optional<std::uint64_t> {
        const auto it = s.find(key);
        if (it != s.end() && it->is_number_unsigned()) {
            return it->get<std::uint64_t>();
        }
        return std::nullopt;
    };
    settings.wrap = {mode("wrapS"), mode("wrapT")};
    settings.filter = filterFromGl(filter("magFilter"), filter("minFilter"));
    return settings;
}

// The image each texture shows, warning once per texture with an alternative.
std::optional<std::size_t> textureImage(const Json& json, std::size_t texture, common::Diagnostics& diagnostics,
                                        std::set<std::size_t>& warned) {
    if (!json.contains("textures") || texture >= json["textures"].size()) {
        return std::nullopt;
    }
    const gltf::TextureSource source = gltf::textureSource(json["textures"][texture]);
    if (!source.alternatives.empty() && !source.from_extension && warned.insert(texture).second) {
        diagnostics.warn("texture-alternative-source",
                         "A texture has both a core source and an extension alternative (WebP, KTX2, AVIF): the core "
                         "source is the one measured",
                         "texture " + std::to_string(texture));
    }
    return source.image;
}

// The materials the scene's primitives use, by index.
std::set<std::size_t> sceneMaterials(const cgltf_data& data, const gltf::Selection& selection) {
    std::set<std::size_t> materials;
    for (std::size_t m = 0; m < data.meshes_count; ++m) {
        if (!selection.isKept(Kind::mesh, m)) {
            continue;
        }
        for (std::size_t p = 0; p < data.meshes[m].primitives_count; ++p) {
            const cgltf_material* material = data.meshes[m].primitives[p].material;
            if (material != nullptr) {
                materials.insert(static_cast<std::size_t>(material - data.materials));
            }
        }
    }
    return materials;
}

} // namespace

Report analyzeDocument(const gltf::Document& document, const gltf::Selection& selection,
                       common::Diagnostics& diagnostics) {
    const cgltf_data& data = document.data();
    const Json& json = document.json();
    Report report;

    for (const gltf::InputFile& file : document.files()) {
        report.files.push_back({file.role, file.name, file.size});
    }
    report.extensions_used = stringArray(json, "extensionsUsed");
    report.extensions_required = stringArray(json, "extensionsRequired");
    for (const std::string& name : report.extensions_required) {
        if (!isUnderstoodExtension(name)) {
            diagnostics.warn("unknown-required-extension",
                             "The model requires an extension meshtinker doesn't know: figures may not match what a "
                             "renderer supporting it shows",
                             name);
        }
    }

    if (selection.scene) {
        Report::Scene scene;
        scene.index = *selection.scene;
        const Json& scene_json = json["scenes"][scene.index];
        scene.name = nonEmptyName(scene_json);
        if (scene_json.contains("nodes") && scene_json["nodes"].is_array()) {
            for (const Json& node : scene_json["nodes"]) {
                if (node.is_number_unsigned()) {
                    scene.first_named_root_node = nonEmptyName(json["nodes"][node.get<std::size_t>()]);
                    if (scene.first_named_root_node) {
                        break;
                    }
                }
            }
        }
        report.scene = scene;
    }

    // Each mesh counts once, however many nodes use it, and each POSITION accessor once.
    std::set<const cgltf_accessor*> positions;
    for (std::size_t m = 0; m < data.meshes_count; ++m) {
        if (!selection.isKept(Kind::mesh, m)) {
            continue;
        }
        ++report.geometry.meshes;
        const cgltf_mesh& mesh = data.meshes[m];
        for (std::size_t p = 0; p < mesh.primitives_count; ++p) {
            const cgltf_primitive& primitive = mesh.primitives[p];
            ++report.geometry.primitives;
            const cgltf_accessor* position = findAttribute(primitive, cgltf_attribute_type_position, 0);
            if (position != nullptr && positions.insert(position).second) {
                report.geometry.vertices += position->count;
            }
            report.geometry.faces += triangleCount(primitive.type, cornerCount(primitive));
            bool quantized = false;
            for (std::size_t a = 0; a < primitive.attributes_count; ++a) {
                quantized = quantized || isQuantized(primitive.attributes[a]);
            }
            if (gltf::GeometryReader::isDraco(primitive)) {
                ++report.geometry.draco;
            } else if (usesMeshopt(primitive)) {
                ++report.geometry.meshopt;
            } else if (quantized) {
                ++report.geometry.quantized;
            } else {
                ++report.geometry.plain;
            }
        }
    }

    // Images: every kept image, with the slots of the scene's materials that show it.
    std::map<std::size_t, std::vector<Report::Use>> uses;
    std::set<std::size_t> warned;
    for (const std::size_t material : sceneMaterials(data, selection)) {
        for (const gltf::TextureSlot& slot : gltf::textureSlots(json["materials"][material])) {
            const auto texture = (*slot.info)["index"].get<std::size_t>();
            const std::optional<std::size_t> image = textureImage(json, texture, diagnostics, warned);
            if (image) {
                uses[*image].push_back({material, slot.path, texture});
            }
        }
    }
    for (std::size_t i = 0; i < data.images_count; ++i) {
        if (!selection.isKept(Kind::image, i)) {
            continue;
        }
        const gltf::ImageData& source = document.image(i);
        Report::Image image;
        image.index = i;
        image.name = nonEmptyName(json["images"][i]);
        if (source.file) {
            image.file = document.files()[*source.file].name;
        }
        image.byte_size = source.bytes.size;
        const auto found_uses = uses.find(i);
        if (source.missing) {
            image.file = source.reference;
            // The base color carries what the model looks like: without it the
            // model can't be shown as intended. Other maps only refine it.
            const bool diffuse = found_uses != uses.end() &&
                                 std::any_of(found_uses->second.begin(), found_uses->second.end(),
                                             [](const Report::Use& use) { return isDiffuseSlot(use.slot); });
            if (diffuse) {
                throw common::rejected("missing-base-color-image", "image '" + source.reference +
                                                                       "', used as a base color: " +
                                                                       common::refused_reference_message);
            }
            diagnostics.warn("missing-image",
                             "An image file is missing, or outside the model's folder: its size and occupancy are "
                             "unknown",
                             source.reference);
        } else if (source.header) {
            image.mime_type = gltf::mimeType(source.header->format);
            if (source.header->ktx2_codec) {
                image.codec = gltf::codecName(*source.header->ktx2_codec);
            }
            image.width = source.header->width;
            image.height = source.header->height;
        } else {
            diagnostics.warn("image-unrecognized",
                             "An image is not PNG, JPEG, WebP, KTX2 or AVIF, or is truncated: its size is unknown",
                             image.file.value_or("image " + std::to_string(i)));
        }
        if (found_uses != uses.end()) {
            image.uses = found_uses->second;
            if (image.width && image.height) {
                report.texels_total += std::uint64_t{*image.width} * *image.height;
            }
        }
        report.images.push_back(std::move(image));
    }
    return report;
}

void analyzeData(gltf::Document& document, const gltf::Selection& selection, Report& report,
                 common::Workspace& workspace, common::Diagnostics& diagnostics) {
    const cgltf_data& data = document.data();
    const Json& json = document.json();
    gltf::GeometryReader reader(document, workspace);

    const auto unreadable = [&](gltf::Readability status, const std::string& where) {
        switch (status) {
        case gltf::Readability::readable:
            break;
        case gltf::Readability::draco_failed:
            diagnostics.warn("draco-decode-failed",
                             "Draco-compressed data could not be decoded, or doesn't match its accessors: bounds and "
                             "occupancy are not computed",
                             where);
            break;
        case gltf::Readability::no_data:
            diagnostics.warn("missing-vertex-data", "Vertex data is missing: bounds and occupancy are not computed",
                             where);
            break;
        case gltf::Readability::meshopt_failed:
            diagnostics.warn("meshopt-decode-failed",
                             "meshopt-compressed data could not be decoded: bounds and occupancy are not computed", where);
            break;
        }
    };

    // Bounds: every vertex the scene's primitives reference, through each node's world matrix.
    if (report.scene) {
        double low[3] = {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::infinity()};
        double high[3] = {-low[0], -low[1], -low[2]};
        bool exact = true;
        bool any = false;
        const auto visitNode = [&](const auto& self, const cgltf_node& node, const Matrix& parent, int depth) -> void {
            if (depth > 1024) {
                return; // cycles are invalid glTF; cgltf_validate rejects them
            }
            const Matrix world = multiply(parent, localMatrix(node));
            if (node.mesh != nullptr) {
                const std::size_t mesh_index = indexOf(data, node.mesh);
                // EXT_mesh_gpu_instancing draws the mesh once per instance, at world * instance.
                const Instancing instances = instancing(node);
                bool instances_readable = true;
                for (const cgltf_accessor* accessor : {instances.translation, instances.rotation, instances.scale}) {
                    if (accessor == nullptr || !instances_readable) {
                        continue;
                    }
                    const gltf::Readability status = reader.prepare(*accessor);
                    if (status != gltf::Readability::readable) {
                        unreadable(status, "node " + std::to_string(static_cast<std::size_t>(&node - data.nodes)) +
                                               " instances");
                        exact = false;
                        instances_readable = false;
                    }
                }
                for (std::size_t p = 0; instances_readable && p < node.mesh->primitives_count; ++p) {
                    const cgltf_primitive& primitive = node.mesh->primitives[p];
                    const cgltf_accessor* position = findAttribute(primitive, cgltf_attribute_type_position, 0);
                    if (position == nullptr) {
                        continue;
                    }
                    gltf::Readability status = reader.preparePrimitive(mesh_index, p);
                    if (status == gltf::Readability::readable) {
                        status = reader.prepare(*position);
                    }
                    if (status == gltf::Readability::readable && primitive.indices != nullptr) {
                        status = reader.prepare(*primitive.indices);
                    }
                    if (status != gltf::Readability::readable) {
                        unreadable(status, primitiveName(document, mesh_index, p));
                        exact = false;
                        continue;
                    }
                    for (std::size_t instance = 0; instance < instances.count; ++instance) {
                        const Matrix m = node.has_mesh_gpu_instancing ? multiply(world, instanceMatrix(instances, instance))
                                                                      : world;
                        const auto include = [&](std::size_t vertex) {
                            if (vertex >= position->count) {
                                return;
                            }
                            float p3[3] = {0, 0, 0};
                            cgltf_accessor_read_float(position, vertex, p3, 3);
                            for (std::size_t a = 0; a < 3; ++a) {
                                const double value = m[a] * p3[0] + m[4 + a] * p3[1] + m[8 + a] * p3[2] + m[12 + a];
                                low[a] = std::min(low[a], value);
                                high[a] = std::max(high[a], value);
                            }
                            any = true;
                        };
                        if (primitive.indices != nullptr) {
                            for (std::size_t i = 0; i < primitive.indices->count; ++i) {
                                include(cgltf_accessor_read_index(primitive.indices, i));
                            }
                        } else {
                            for (std::size_t v = 0; v < position->count; ++v) {
                                include(v);
                            }
                        }
                    }
                }
            }
            for (std::size_t c = 0; c < node.children_count; ++c) {
                self(self, *node.children[c], world, depth + 1);
            }
        };
        const cgltf_scene& scene = data.scenes[report.scene->index];
        for (std::size_t n = 0; n < scene.nodes_count; ++n) {
            visitNode(visitNode, *scene.nodes[n], identity, 0);
        }
        if (exact && any) {
            report.scene->bounds = Report::Bounds{{low[0], low[1], low[2]}, {high[0], high[1], high[2]}};
        }
    }

    // Occupancy: each mesh once, each textured slot of its material. Images of
    // the same size drawn the same way (same primitives, UV set, transform and
    // sampler), such as a material's base color, normal and occlusion maps,
    // share one coverage map, so their triangles are rasterized once.
    std::map<std::size_t, Report::Image*> images;
    for (Report::Image& image : report.images) {
        if (!image.uses.empty() && image.width && image.height && *image.width > 0 && *image.height > 0) {
            images[image.index] = &image;
        }
    }
    std::map<std::size_t, std::set<Draw>> draws_of;
    std::set<std::size_t> warned;
    common::Diagnostics ignored;
    for (std::size_t m = 0; m < data.meshes_count; ++m) {
        if (!selection.isKept(Kind::mesh, m)) {
            continue;
        }
        const cgltf_mesh& mesh = data.meshes[m];
        for (std::size_t p = 0; p < mesh.primitives_count; ++p) {
            const cgltf_primitive& primitive = mesh.primitives[p];
            if (primitive.material == nullptr || triangleCount(primitive.type, cornerCount(primitive)) == 0) {
                continue;
            }
            const auto material = static_cast<std::size_t>(primitive.material - data.materials);
            for (const gltf::TextureSlot& slot : gltf::textureSlots(json["materials"][material])) {
                const auto texture = (*slot.info)["index"].get<std::size_t>();
                const std::optional<std::size_t> image = textureImage(json, texture, ignored, warned);
                if (!image || images.count(*image) == 0) {
                    continue;
                }
                Draw draw;
                draw.mesh = m;
                draw.primitive = p;
                draw.transform = uvTransform(*slot.info);
                draw.set = draw.transform.tex_coord.value_or(slot.info->value("texCoord", std::uint64_t{0}));
                draw.sampler = samplerSettings(json, json["textures"][texture]);
                draws_of[*image].insert(draw);
            }
        }
    }
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::set<Draw>>, std::vector<std::size_t>> groups;
    for (const auto& [image, draws] : draws_of) {
        groups[{*images[image]->width, *images[image]->height, draws}].push_back(image);
    }
    std::set<std::size_t> incomplete;
    std::map<std::size_t, std::uint64_t> used;
    std::uint64_t repeats_skipped = 0;
    std::uint64_t invalid_skipped = 0;
    std::size_t group_index = 0;
    for (const auto& [key, members] : groups) {
        const auto& [width, height, draws] = key;
        Coverage map(workspace, "coverage-" + std::to_string(group_index++), width, height);
        bool complete = true;
        for (const Draw& draw : draws) {
            const cgltf_primitive& primitive = data.meshes[draw.mesh].primitives[draw.primitive];
            const std::string where = primitiveName(document, draw.mesh, draw.primitive);
            if (const gltf::Readability decoded = reader.preparePrimitive(draw.mesh, draw.primitive);
                decoded != gltf::Readability::readable) {
                unreadable(decoded, where);
                complete = false;
                continue;
            }
            const cgltf_accessor* uv = findAttribute(primitive, cgltf_attribute_type_texcoord, static_cast<int>(draw.set));
            if (uv == nullptr) {
                diagnostics.warn("missing-texture-coordinates",
                                 "A textured primitive has no texture coordinates for its texture: it is left out of "
                                 "occupancy",
                                 where + ", TEXCOORD_" + std::to_string(draw.set));
                continue;
            }
            gltf::Readability status = reader.prepare(*uv);
            if (status == gltf::Readability::readable && primitive.indices != nullptr) {
                status = reader.prepare(*primitive.indices);
            }
            if (status != gltf::Readability::readable) {
                unreadable(status, where);
                complete = false;
                continue;
            }
            const std::uint64_t triangles = triangleCount(primitive.type, cornerCount(primitive));
            for (std::uint64_t t = 0; t < triangles; ++t) {
                std::array<std::array<double, 2>, 3> tri{};
                bool valid = true;
                const std::array<std::uint64_t, 3> ks = triangleCorners(primitive.type, t);
                for (std::size_t c = 0; c < 3; ++c) {
                    const std::size_t vertex = primitive.indices != nullptr
                                                   ? cgltf_accessor_read_index(primitive.indices, ks[c])
                                                   : static_cast<std::size_t>(ks[c]);
                    float value[2] = {0, 0};
                    valid = valid && vertex < uv->count && cgltf_accessor_read_float(uv, vertex, value, 2);
                    tri[c] = draw.transform.apply(value[0], value[1]);
                }
                if (!valid) {
                    ++invalid_skipped;
                    continue;
                }
                switch (map.triangle(tri, draw.sampler.wrap, draw.sampler.filter)) {
                case Coverage::Result::drawn:
                    break;
                case Coverage::Result::too_many_repeats:
                    ++repeats_skipped;
                    break;
                case Coverage::Result::invalid:
                    ++invalid_skipped;
                    break;
                }
            }
        }
        const std::uint64_t count = map.count();
        for (const std::size_t image : members) {
            used[image] = count;
            if (!complete) {
                incomplete.insert(image);
            }
        }
    }
    if (repeats_skipped > 0) {
        diagnostics.add(common::Severity::warning, "texture-repeats-skipped",
                        "UV triangles repeating their texture more than 64 times vertically were left out of occupancy",
                        std::to_string(repeats_skipped) + " triangles");
    }
    if (invalid_skipped > 0) {
        diagnostics.add(common::Severity::warning, "invalid-texture-coordinates",
                        "UV triangles with indices out of range or coordinates that aren't finite were left out of "
                        "occupancy",
                        std::to_string(invalid_skipped) + " triangles");
    }
    std::uint64_t used_total = 0;
    bool all_known = true;
    for (auto& [index, image] : images) {
        if (incomplete.count(index) != 0) {
            all_known = false;
            continue;
        }
        const auto found = used.find(index);
        image->used_texels = found == used.end() ? 0 : found->second;
        image->occupancy = static_cast<double>(image->used_texels) /
                           (static_cast<double>(*image->width) * static_cast<double>(*image->height));
        used_total += image->used_texels;
    }
    if (all_known && report.texels_total > 0) {
        report.texels_occupancy = static_cast<double>(used_total) / static_cast<double>(report.texels_total);
    }
}

Report outputReport(const Report& input, const gltf::Layout& layout, const std::vector<gltf::WrittenFile>& files,
                    const std::vector<std::string>& image_names) {
    Report output = input;
    output.files.clear();
    for (const gltf::WrittenFile& file : files) {
        output.files.push_back({file.role, file.name, file.size});
    }
    const auto remap = [&](Kind kind, std::size_t index) {
        return layout.remap[static_cast<std::size_t>(kind)][index];
    };
    if (output.scene) {
        output.scene->index = remap(Kind::scene, output.scene->index);
    }
    for (Report::Image& image : output.images) {
        const std::size_t source = image.index;
        image.index = remap(Kind::image, source);
        image.file = image_names[source];
        for (Report::Use& use : image.uses) {
            use.material = remap(Kind::material, use.material);
            use.texture = remap(Kind::texture, use.texture);
        }
    }
    return output;
}

} // namespace meshtinker::analysis
