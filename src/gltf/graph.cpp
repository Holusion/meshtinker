#include "meshtinker/gltf/graph.hpp"

#include <algorithm>
#include <cstring>

namespace meshtinker::gltf {
namespace {

constexpr const char* texture_source_extensions[] = {"EXT_texture_webp", "KHR_texture_basisu", "EXT_texture_avif",
                                                     "MSFT_texture_dds"};
constexpr const char* meshopt_extensions[] = {"EXT_meshopt_compression", "KHR_meshopt_compression"};

bool endsWith(const std::string& text, const char* suffix) {
    const std::size_t length = std::strlen(suffix);
    return text.size() >= length && text.compare(text.size() - length, length, suffix) == 0;
}

// Works on Json and const Json alike.
template <typename J, typename Visit>
void index(J& object, const char* key, Kind kind, Visit& visit) {
    if (!object.is_object()) {
        return;
    }
    auto found = object.find(key);
    if (found != object.end() && found->is_number_integer()) {
        visit(kind, *found);
    }
}

template <typename J, typename Visit>
void indexArray(J& object, const char* key, Kind kind, Visit& visit) {
    if (!object.is_object()) {
        return;
    }
    auto found = object.find(key);
    if (found == object.end() || !found->is_array()) {
        return;
    }
    for (auto& element : *found) {
        if (element.is_number_integer()) {
            visit(kind, element);
        }
    }
}

template <typename J, typename Visit>
void indexValues(J& object, const char* key, Kind kind, Visit& visit) {
    if (!object.is_object()) {
        return;
    }
    auto found = object.find(key);
    if (found == object.end() || !found->is_object()) {
        return;
    }
    for (auto& element : found->items()) {
        if (element.value().is_number_integer()) {
            visit(kind, element.value());
        }
    }
}

template <typename J>
J* child(J& object, const char* key) {
    if (!object.is_object()) {
        return nullptr;
    }
    auto found = object.find(key);
    return found == object.end() ? nullptr : &*found;
}

template <typename J>
J* extension(J& object, const char* name) {
    J* extensions = child(object, "extensions");
    return extensions == nullptr ? nullptr : child(*extensions, name);
}

// Every `*Texture` object with an `index` under a material extension, at any depth.
template <typename J, typename Visit>
void extensionTextures(J& value, Visit& visit) {
    if (value.is_object()) {
        for (auto& item : value.items()) {
            if (endsWith(item.key(), "Texture") && item.value().is_object()) {
                index(item.value(), "index", Kind::texture, visit);
            }
            extensionTextures(item.value(), visit);
        }
    } else if (value.is_array()) {
        for (auto& element : value) {
            extensionTextures(element, visit);
        }
    }
}

template <typename J, typename Visit>
void visitReferences(Kind owner, J& object, Visit& visit) {
    switch (owner) {
    case Kind::scene:
        indexArray(object, "nodes", Kind::node, visit);
        break;
    case Kind::node:
        indexArray(object, "children", Kind::node, visit);
        index(object, "mesh", Kind::mesh, visit);
        index(object, "skin", Kind::skin, visit);
        index(object, "camera", Kind::camera, visit);
        if (J* instancing = extension(object, "EXT_mesh_gpu_instancing")) {
            indexValues(*instancing, "attributes", Kind::accessor, visit);
        }
        if (J* lod = extension(object, "MSFT_lod")) {
            indexArray(*lod, "ids", Kind::node, visit);
        }
        break;
    case Kind::mesh:
        if (J* primitives = child(object, "primitives"); primitives != nullptr && primitives->is_array()) {
            for (auto& primitive : *primitives) {
                indexValues(primitive, "attributes", Kind::accessor, visit);
                index(primitive, "indices", Kind::accessor, visit);
                index(primitive, "material", Kind::material, visit);
                if (J* targets = child(primitive, "targets"); targets != nullptr && targets->is_array()) {
                    for (auto& target : *targets) {
                        if (target.is_object()) {
                            for (auto& attribute : target.items()) {
                                if (attribute.value().is_number_integer()) {
                                    visit(Kind::accessor, attribute.value());
                                }
                            }
                        }
                    }
                }
                if (J* draco = extension(primitive, "KHR_draco_mesh_compression")) {
                    index(*draco, "bufferView", Kind::buffer_view, visit);
                }
                if (J* variants = extension(primitive, "KHR_materials_variants")) {
                    if (J* mappings = child(*variants, "mappings"); mappings != nullptr && mappings->is_array()) {
                        for (auto& mapping : *mappings) {
                            index(mapping, "material", Kind::material, visit);
                        }
                    }
                }
            }
        }
        break;
    case Kind::skin:
        index(object, "inverseBindMatrices", Kind::accessor, visit);
        index(object, "skeleton", Kind::node, visit);
        indexArray(object, "joints", Kind::node, visit);
        break;
    case Kind::material:
        if (J* pbr = child(object, "pbrMetallicRoughness")) {
            for (const char* slot : {"baseColorTexture", "metallicRoughnessTexture"}) {
                if (J* info = child(*pbr, slot)) {
                    index(*info, "index", Kind::texture, visit);
                }
            }
        }
        for (const char* slot : {"normalTexture", "occlusionTexture", "emissiveTexture"}) {
            if (J* info = child(object, slot)) {
                index(*info, "index", Kind::texture, visit);
            }
        }
        if (J* extensions = child(object, "extensions"); extensions != nullptr && extensions->is_object()) {
            for (auto& item : extensions->items()) {
                if (item.key() == "MSFT_lod") {
                    indexArray(item.value(), "ids", Kind::material, visit);
                } else {
                    extensionTextures(item.value(), visit);
                }
            }
        }
        break;
    case Kind::texture:
        index(object, "source", Kind::image, visit);
        index(object, "sampler", Kind::sampler, visit);
        for (const char* name : texture_source_extensions) {
            if (J* source = extension(object, name)) {
                index(*source, "source", Kind::image, visit);
            }
        }
        break;
    case Kind::image:
        index(object, "bufferView", Kind::buffer_view, visit);
        break;
    case Kind::accessor:
        index(object, "bufferView", Kind::buffer_view, visit);
        if (J* sparse = child(object, "sparse")) {
            if (J* indices = child(*sparse, "indices")) {
                index(*indices, "bufferView", Kind::buffer_view, visit);
            }
            if (J* values = child(*sparse, "values")) {
                index(*values, "bufferView", Kind::buffer_view, visit);
            }
        }
        break;
    case Kind::buffer_view:
        index(object, "buffer", Kind::buffer, visit);
        for (const char* name : meshopt_extensions) {
            if (J* meshopt = extension(object, name)) {
                index(*meshopt, "buffer", Kind::buffer, visit);
            }
        }
        break;
    case Kind::animation:
        if (J* channels = child(object, "channels"); channels != nullptr && channels->is_array()) {
            for (auto& channel : *channels) {
                if (J* target = child(channel, "target")) {
                    index(*target, "node", Kind::node, visit);
                }
            }
        }
        if (J* samplers = child(object, "samplers"); samplers != nullptr && samplers->is_array()) {
            for (auto& sampler : *samplers) {
                index(sampler, "input", Kind::accessor, visit);
                index(sampler, "output", Kind::accessor, visit);
            }
        }
        break;
    case Kind::camera:
    case Kind::sampler:
    case Kind::buffer:
        break;
    }
}

} // namespace

const char* arrayName(Kind kind) {
    switch (kind) {
    case Kind::scene:
        return "scenes";
    case Kind::node:
        return "nodes";
    case Kind::mesh:
        return "meshes";
    case Kind::skin:
        return "skins";
    case Kind::camera:
        return "cameras";
    case Kind::material:
        return "materials";
    case Kind::texture:
        return "textures";
    case Kind::sampler:
        return "samplers";
    case Kind::image:
        return "images";
    case Kind::accessor:
        return "accessors";
    case Kind::buffer_view:
        return "bufferViews";
    case Kind::buffer:
        return "buffers";
    case Kind::animation:
        return "animations";
    }
    return "";
}

void forEachReference(Kind owner, Json& object, const std::function<void(Kind, Json&)>& visit) {
    visitReferences(owner, object, visit);
}

void forEachReference(Kind owner, const Json& object, const std::function<void(Kind, const Json&)>& visit) {
    visitReferences(owner, object, visit);
}

bool isIndexAwareExtension(const std::string& name) {
    // Known to forEachReference, or holding no index into a top-level array.
    static const char* const known[] = {
        "EXT_mesh_gpu_instancing", "MSFT_lod", "KHR_draco_mesh_compression", "KHR_materials_variants",
        "EXT_meshopt_compression", "KHR_meshopt_compression", "EXT_texture_webp", "KHR_texture_basisu",
        "EXT_texture_avif", "MSFT_texture_dds", "KHR_texture_transform", "KHR_mesh_quantization",
        "KHR_lights_punctual", "KHR_materials_emissive_strength", "KHR_materials_unlit", "KHR_xmp_json_ld",
        "KHR_texture_basisu", "KHR_materials_pbrSpecularGlossiness", "KHR_materials_clearcoat",
        "KHR_materials_ior", "KHR_materials_specular", "KHR_materials_transmission", "KHR_materials_volume",
        "KHR_materials_sheen", "KHR_materials_iridescence", "KHR_materials_anisotropy",
        "KHR_materials_dispersion", "KHR_materials_diffuse_transmission", "EXT_lights_image_based"};
    return std::find(std::begin(known), std::end(known), name) != std::end(known);
}

std::vector<TextureSlot> textureSlots(const Json& material) {
    std::vector<TextureSlot> slots;
    const auto add = [&](const std::string& path, const Json* info) {
        if (info != nullptr && info->is_object() && info->contains("index") && (*info)["index"].is_number_integer()) {
            slots.push_back({path, info});
        }
    };
    if (const Json* pbr = child(material, "pbrMetallicRoughness")) {
        add("baseColorTexture", child(*pbr, "baseColorTexture"));
        add("metallicRoughnessTexture", child(*pbr, "metallicRoughnessTexture"));
    }
    for (const char* slot : {"normalTexture", "occlusionTexture", "emissiveTexture"}) {
        add(slot, child(material, slot));
    }
    const Json* extensions = child(material, "extensions");
    if (extensions != nullptr && extensions->is_object()) {
        std::function<void(const std::string&, const Json&)> walk = [&](const std::string& prefix, const Json& value) {
            if (!value.is_object()) {
                return;
            }
            for (const auto& item : value.items()) {
                const std::string path = prefix + "." + item.key();
                if (endsWith(item.key(), "Texture")) {
                    add(path, &item.value());
                } else {
                    walk(path, item.value());
                }
            }
        };
        for (const auto& item : extensions->items()) {
            if (item.key() != "MSFT_lod") {
                walk(item.key(), item.value());
            }
        }
    }
    return slots;
}

TextureSource textureSource(const Json& texture) {
    TextureSource result;
    const auto indexOf = [](const Json& object) -> std::optional<std::size_t> {
        const auto found = object.find("source");
        if (found != object.end() && found->is_number_unsigned()) {
            return found->get<std::size_t>();
        }
        return std::nullopt;
    };
    if (texture.is_object()) {
        result.image = indexOf(texture);
        for (const char* name : texture_source_extensions) {
            const Json* source = extension(texture, name);
            if (source == nullptr || !source->is_object()) {
                continue;
            }
            const std::optional<std::size_t> image = indexOf(*source);
            if (!image) {
                continue;
            }
            if (!result.image) {
                result.image = image;
                result.from_extension = true;
            } else if (*image != *result.image) {
                result.alternatives.push_back(*image);
            }
        }
    }
    return result;
}

} // namespace meshtinker::gltf
