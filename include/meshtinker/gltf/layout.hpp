#pragma once

#include "meshtinker/common/bytes.hpp"
#include "meshtinker/common/diagnostics.hpp"
#include "meshtinker/gltf/document.hpp"
#include "meshtinker/gltf/selection.hpp"

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace meshtinker::gltf {

// The document to write, before its binary data is placed.
struct Layout {
    // The output JSON. Indices are already renumbered, but buffer views still
    // name their source buffer and offsets, and images their source: the
    // writer rewrites those.
    Json json;
    // Output buffer view → source buffer view.
    std::vector<std::size_t> view_sources;
    // Output image → source image.
    std::vector<std::size_t> image_sources;
    // Output buffer views in the order their bytes go into the .bin.
    std::vector<std::size_t> bin_order;
    // By kind, then by source index: the output index, or `dropped`.
    std::array<std::vector<std::size_t>, kind_count> remap;
    // The bytes of each output buffer view a command added (gltf/edit.hpp),
    // whose view_sources entry is `added`. They must outlive the write. For a
    // view with an EXT_ or KHR_meshopt_compression extension, they are the
    // compressed bytes, and the view's own range goes in a fallback buffer.
    std::map<std::size_t, common::ByteView> added;
    // An image a command wrote itself, as it encoded it (gltf/edit.hpp).
    struct AddedImage {
        // The file's name in the output directory.
        std::string name;
        std::uint64_t size = 0;
    };
    // The images a command added, by output image, whose image_sources entry
    // is `added_image`. The writer points them at their file and doesn't
    // write it.
    std::map<std::size_t, AddedImage> added_images;
    static constexpr std::size_t dropped = static_cast<std::size_t>(-1);
    static constexpr std::size_t added_view = dropped - 1;
    static constexpr std::size_t added_image = dropped - 1;
};

// The .bin order of the profile for `json`'s buffer views: those primitives
// use, in primitive order (meshes in order, then their primitives: indices,
// attributes, morph targets, Draco data), then every other view in index order.
std::vector<std::size_t> binOrder(const Json& json);

// Every object kept, indices unchanged, buffer views in source order.
Layout identityLayout(const Document& document);

// The normalized profile's layout (docs/normalized-profile.md):
// - only the selection's objects, in source order, with every index
//   forEachReference knows renumbered, and the kept scene as scene 0;
// - buffer views that only images used are dropped, since images become files;
// - in the .bin, buffer views in the order primitives use them (meshes in
//   order, then their primitives: indices, attributes, morph targets, Draco
//   data), then every other kept view in index order.
//
// When anything is renumbered and the document uses an extension
// isIndexAwareExtension doesn't know, its indices may be stale: one
// `extension-indices-not-renumbered` warning per such extension.
Layout normalizedLayout(const Document& document, const Selection& selection, common::Diagnostics& diagnostics);

// Reads `path` as every command after `import` reads its input
// (docs/normalized-profile.md, "How commands get normalized input"): the
// normalized layout of the file, as a Document (Document::normalized). Its
// indices are those of the file `import` writes; its buffers and byte offsets
// are the input's. Warns as normalizedLayout does.
Document readNormalized(const std::filesystem::path& path, const ReadOptions& options,
                        common::Diagnostics& diagnostics);

} // namespace meshtinker::gltf
