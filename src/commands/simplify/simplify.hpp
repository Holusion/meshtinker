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

namespace meshtinker::simplify {

constexpr double default_error = 0.001;

struct Options {
    // The largest distance a surface may move, as a fraction of its mesh's
    // size (the longest side of the mesh's bounding box).
    double error = default_error;
    gltf::ReadOptions read;
};

struct Counts {
    std::uint64_t primitives = 0;
    std::uint64_t vertices = 0;
    std::uint64_t faces = 0;
};

struct Report {
    double target_error = 0;
    // The largest error meshoptimizer reports for a simplified primitive,
    // relative to its mesh's size.
    double achieved_error = 0;
    // Triangle primitives that were read, each counted once however many
    // nodes show its mesh.
    Counts input;
    Counts output;
    std::uint64_t simplified = 0;
    std::uint64_t unchanged = 0;
    // Primitives copied as they are: points, lines, unreadable data.
    std::uint64_t skipped = 0;
    std::vector<gltf::WrittenFile> files;
};

// Simplifies the triangles of `input` and writes the model as `<stem>.gltf`,
// `<stem>.bin` and its images in `directory` (docs/simplify.md).
Report simplify(const std::filesystem::path& input, const std::filesystem::path& directory, const std::string& stem,
                const Options& options, common::Workspace& workspace, common::Outputs& outputs,
                common::Diagnostics& diagnostics);

nlohmann::ordered_json toJson(const Report& report, nlohmann::ordered_json header, const common::Diagnostics& diagnostics);
void printText(std::ostream& out, const Report& report);

} // namespace meshtinker::simplify
