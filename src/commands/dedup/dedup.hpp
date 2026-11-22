#pragma once

#include "meshtinker/common/diagnostics.hpp"
#include "meshtinker/common/outputs.hpp"
#include "meshtinker/gltf/document.hpp"
#include "meshtinker/gltf/writer.hpp"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace meshtinker::dedup {

struct Options {
    gltf::ReadOptions read;
    // Count what would merge, and write nothing (the hidden `--dry-run`, for
    // surveys of large models).
    bool dry_run = false;
};

// How many objects of one kind the model had, and has once duplicates are merged.
struct Count {
    std::uint64_t before = 0;
    std::uint64_t after = 0;
};

struct Report {
    Count images;
    Count samplers;
    Count textures;
    Count materials;
    Count accessors;
    Count meshes;
    // The bytes the model stores: buffer views (`byteLength`), and images,
    // each file or embedded image once. An accessor merged into another frees
    // its bytes only when no other accessor uses its buffer view.
    Count view_bytes;
    Count image_bytes;
    std::vector<gltf::WrittenFile> files;
};

// Stores identical images, samplers, textures, materials, accessors and
// meshes of `input` once, and writes the model as `<stem>.gltf`, `<stem>.bin`
// and its images in `directory` (docs/dedup.md).
Report dedup(const std::filesystem::path& input, const std::filesystem::path& directory, const std::string& stem,
             const Options& options, common::Outputs& outputs, common::Diagnostics& diagnostics);

nlohmann::ordered_json toJson(const Report& report, nlohmann::ordered_json header, const common::Diagnostics& diagnostics);
void printText(std::ostream& out, const Report& report);

} // namespace meshtinker::dedup
