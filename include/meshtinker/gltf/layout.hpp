#pragma once

#include "meshtinker/common/diagnostics.hpp"
#include "meshtinker/gltf/document.hpp"
#include "meshtinker/gltf/selection.hpp"

#include <cstddef>
#include <filesystem>
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
    static constexpr std::size_t dropped = static_cast<std::size_t>(-1);
};

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
