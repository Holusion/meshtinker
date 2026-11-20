#pragma once

#include "meshtinker/common/outputs.hpp"
#include "meshtinker/gltf/document.hpp"
#include "meshtinker/gltf/layout.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace meshtinker::gltf {

// A file writeSeparate created, in the shape of Document::files().
struct WrittenFile {
    FileRole role;
    std::string name;
    std::uint64_t size;
};

// The name each image is written under, by image index. Images that share a
// source file share a name.
//
// - An image read from a file keeps that file's name, without its folders. So
//   does a missing image file, which the writer then doesn't write.
// - An image from a buffer view or data URI is `<stem>_texture_<index>.<ext>`,
//   with the extension of its format.
// - A name that collides with an earlier one, or with `<stem>.gltf` or
//   `<stem>.bin`, ignoring case (for case-insensitive file systems), becomes
//   `<stem>_texture_<index>.<ext>` too, with a further `_<n>` if needed.
std::vector<std::string> imageFileNames(const Document& document, const std::string& stem);

// Writes `layout` as separate files in `directory`: `<stem>.gltf`, one
// `<stem>.bin` holding the bytes of every buffer view, and one file per image.
// `document` provides the bytes. Returns the files written, the .gltf first.
//
// The layout's JSON passes through, except for:
// - `buffers`: the .bin first (left out when there are no bytes), then each
//   buffer without data (meshopt fallbacks) that a view still uses, unchanged.
//   Names, extras and extensions of buffers merged into the .bin are not kept.
// - `bufferViews`: `buffer` and `byteOffset`, and the same two properties of
//   an EXT/KHR_meshopt_compression extension, point into the .bin, where views
//   follow `layout.bin_order`, each starting 4-byte aligned.
// - `images`: `uri` names the written file; `bufferView` is removed.
std::vector<WrittenFile> writeSeparate(const Document& document, const Layout& layout,
                                       const std::filesystem::path& directory, const std::string& stem,
                                       common::Outputs& outputs);

// Writes `layout` as one GLB file at `path`. The BIN chunk is buffer 0: the
// buffer views as writeSeparate lays them out in its .bin, then the bytes of
// each image, in a new buffer view at the end of `bufferViews`, with its
// `mimeType`. Images that share a source file share a view. A missing image
// keeps its `uri`. Throws a Failure (exit code 1, `glb-too-large`) for a
// model over the format's 4 GiB.
std::vector<WrittenFile> writeGlb(const Document& document, const Layout& layout, const std::filesystem::path& path,
                                  common::Outputs& outputs);

} // namespace meshtinker::gltf
