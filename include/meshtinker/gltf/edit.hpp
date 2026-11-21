#pragma once

#include "meshtinker/common/bytes.hpp"
#include "meshtinker/gltf/layout.hpp"

#include <cstddef>
#include <optional>
#include <string>

namespace meshtinker::gltf {

// Editing a Layout before it is written, for commands that rewrite geometry
// (docs/gltf-io.md, "Editing"). They add accessors with their bytes, point
// primitives at them, then prune what nothing uses any more.

// Appends a buffer view whose bytes are `bytes`, and returns its index. `view`
// is its JSON without `buffer` and `byteOffset`, which the writer sets. With an
// EXT_ or KHR_meshopt_compression extension, `bytes` are the compressed data
// (see Layout::added). The bytes must outlive the write.
std::size_t addView(Layout& layout, Json view, common::ByteView bytes);

// Appends an image whose file `name` (of `size` bytes) the command has
// written in the output directory, and returns its index. `image` is its JSON
// without `uri` or `bufferView`, which the writer sets. Pick `name` with
// ImageNames (writer.hpp), so that it doesn't collide with another file.
std::size_t addImage(Layout& layout, Json image, std::string name, std::uint64_t size);

// Appends `accessor` and returns its index.
std::size_t addAccessor(Layout& layout, Json accessor);

// What replaces a primitive's geometry.
struct PrimitiveGeometry {
    // Accessor index by attribute name.
    Json attributes = Json::object();
    std::optional<std::size_t> indices;
    // Morph targets, an array of attribute objects, or null for none.
    Json targets;
    int mode = 4;
};

// Points primitive `primitive` of mesh `mesh` at `geometry`: its attributes,
// indices, targets and mode are replaced, and its KHR_draco_mesh_compression
// extension is dropped. Its material, extras and other extensions stay.
void replaceGeometry(Layout& layout, std::size_t mesh, std::size_t primitive, const PrimitiveGeometry& geometry);

// Copies what an accessor holds besides its data (`name`, `extras` and
// `extensions`) from accessor `source` of `layout` onto `accessor`, so that a
// rewritten accessor keeps them. Nothing if `source` is out of range.
void copyAccessorProperties(const Layout& layout, std::size_t source, Json& accessor);

// Adds `name` to extensionsUsed, and to extensionsRequired if `required`.
void useExtension(Json& json, const std::string& name, bool required);

// True if an attribute uses a component type that core glTF doesn't allow for
// it, which KHR_mesh_quantization does: integer positions, normals, tangents
// and morph targets, non-normalized integer texture coordinates, signed
// normalized ones.
bool needsMeshQuantization(const Json& json);

// Removes primitive `primitive` of mesh `mesh`, such as one a command left
// without geometry. prune then removes what nothing uses any more: a command
// that empties a primitive removes it at once (AGENTS.md).
void removePrimitive(Layout& layout, std::size_t mesh, std::size_t primitive);

// Drops what the scene no longer reaches, as import selects it
// (gltf::selectScene): meshes left without primitives (the nodes that drew
// them keep their place, without mesh, morph weights or instancing, and their
// weight animations go), then the materials, textures, samplers, images,
// accessors and buffer views nothing uses. Renumbers the rest (Layout::remap,
// view_sources, image_sources and added follow), and puts the .bin back in the
// profile's order (binOrder). Then drops
// KHR_draco_mesh_compression, EXT_meshopt_compression, KHR_meshopt_compression
// and KHR_mesh_quantization from extensionsUsed and extensionsRequired when no
// primitive, buffer view, buffer or attribute uses them any more.
void prune(Layout& layout);

} // namespace meshtinker::gltf
