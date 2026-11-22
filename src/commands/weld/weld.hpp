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

namespace meshtinker::weld {

struct Options {
    gltf::ReadOptions read;
};

struct Counts {
    std::uint64_t primitives = 0;
    std::uint64_t vertices = 0;
    std::uint64_t faces = 0;
};

struct Report {
    // Triangle primitives that were read, each counted once however many
    // nodes show its mesh.
    Counts input;
    Counts output;
    std::uint64_t welded = 0;
    // Read, but with no two vertices alike and none unused.
    std::uint64_t unchanged = 0;
    // Draco and meshopt primitives, copied as they are: their encoders
    // already merged their vertices.
    std::uint64_t compressed = 0;
    // Copied as they are: points, lines, vertices shared with another
    // primitive, unreadable data.
    std::uint64_t skipped = 0;
    std::vector<gltf::WrittenFile> files;
};

// Merges the vertices of each triangle primitive of `input` that are equal in
// every attribute, and writes the model as `<stem>.gltf`, `<stem>.bin` and its
// images in `directory` (docs/weld.md).
Report weld(const std::filesystem::path& input, const std::filesystem::path& directory, const std::string& stem,
            const Options& options, common::Workspace& workspace, common::Outputs& outputs,
            common::Diagnostics& diagnostics);

nlohmann::ordered_json toJson(const Report& report, nlohmann::ordered_json header, const common::Diagnostics& diagnostics);
void printText(std::ostream& out, const Report& report);

} // namespace meshtinker::weld
