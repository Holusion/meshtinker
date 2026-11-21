#pragma once

#include "meshtinker/common/diagnostics.hpp"
#include "meshtinker/common/outputs.hpp"
#include "meshtinker/common/workspace.hpp"
#include "meshtinker/gltf/document.hpp"
#include "meshtinker/gltf/writer.hpp"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace meshtinker::normals {

// Edges whose faces meet at more than this angle stay hard
// (docs/normals.md#creases).
constexpr double default_crease_angle = 30;

struct Options {
    gltf::ReadOptions read;
    // In degrees, from 0 (every edge hard: flat) to 180 (none).
    double crease_angle = default_crease_angle;
    // Leave primitives whose normals are all flat as they are: only those
    // without normals get them.
    bool keep_flat = false;
};

struct Counts {
    std::uint64_t primitives = 0;
    std::uint64_t vertices = 0;
    std::uint64_t faces = 0;
};

struct Report {
    double crease_angle = default_crease_angle;
    // The primitives whose normals were written, each counted once however
    // many nodes show its mesh: before and after.
    Counts input;
    Counts output;
    // Written, on primitives that had no NORMAL.
    std::uint64_t added = 0;
    // Written over normals that were all flat.
    std::uint64_t smoothed = 0;
    // Triangle primitives whose own normals were kept: authored ones, smooth
    // or split, and flat ones with --keep-flat.
    std::uint64_t kept = 0;
    // Primitives with an unlit material (KHR_materials_unlit), which no
    // viewer shades.
    std::uint64_t unlit = 0;
    // Copied as they are, with a warning: morph targets, unreadable data.
    std::uint64_t skipped = 0;
    std::vector<gltf::WrittenFile> files;
};

// Writes smooth normals on the triangle primitives of `input` that have none,
// or only flat ones, and writes the model as `<stem>.gltf`, `<stem>.bin` and
// its images in `directory` (docs/normals.md).
Report normals(const std::filesystem::path& input, const std::filesystem::path& directory, const std::string& stem,
               const Options& options, common::Workspace& workspace, common::Outputs& outputs,
               common::Diagnostics& diagnostics);

nlohmann::ordered_json toJson(const Report& report, nlohmann::ordered_json header, const common::Diagnostics& diagnostics);
void printText(std::ostream& out, const Report& report);

} // namespace meshtinker::normals
