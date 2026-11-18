#pragma once

#include "meshtinker/gltf/document.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace meshtinker::gltf {

// The top-level glTF arrays whose elements other objects refer to by index.
enum class Kind : std::uint8_t {
    scene,
    node,
    mesh,
    skin,
    camera,
    material,
    texture,
    sampler,
    image,
    accessor,
    buffer_view,
    buffer,
    animation,
};
inline constexpr std::size_t kind_count = 13;

// The JSON array name of a kind: "bufferViews" for Kind::buffer_view.
const char* arrayName(Kind kind);

// Calls `visit(kind, value)` for every index stored in `object`, an element
// of the `owner` array, where `value` is the JSON number to read or rewrite.
// Covers the glTF 2.0 schema and these extensions: EXT_mesh_gpu_instancing,
// MSFT_lod, KHR_draco_mesh_compression, KHR_materials_variants (mappings),
// EXT_meshopt_compression, KHR_meshopt_compression, the texture source
// extensions (EXT_texture_webp, KHR_texture_basisu, EXT_texture_avif,
// MSFT_texture_dds), and every material extension's `*Texture` objects.
// Indices into animation-local arrays (channel samplers) and into extension
// arrays (lights, variants) are not visited: they are never renumbered.
void forEachReference(Kind owner, Json& object, const std::function<void(Kind, Json&)>& visit);
void forEachReference(Kind owner, const Json& object, const std::function<void(Kind, const Json&)>& visit);

// Extensions whose indices forEachReference knows, or that hold none to renumber.
bool isIndexAwareExtension(const std::string& name);

// The texture slots of a material: the property path from the material, such
// as `baseColorTexture` or `KHR_materials_clearcoat.clearcoatTexture`, and the
// textureInfo object. Core slots come first, in schema order, then extension
// slots in document order.
struct TextureSlot {
    std::string path;
    const Json* info;
};
std::vector<TextureSlot> textureSlots(const Json& material);

// The image a texture shows: its core `source`, else the first extension
// source. `alternatives` receives the other images it names.
struct TextureSource {
    std::optional<std::size_t> image;
    // True when the image comes from an extension, not the core `source`.
    bool from_extension = false;
    std::vector<std::size_t> alternatives;
};
TextureSource textureSource(const Json& texture);

} // namespace meshtinker::gltf
