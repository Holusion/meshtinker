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

namespace meshtinker::uv {

struct Options {
    // The longest side of an output image, 0 for no cap (docs/images.md#resizing).
    std::uint32_t max_size = 0;
    // Texels left empty around each chart, besides xatlas's bilinear border.
    std::uint32_t padding = 3;
    // xatlas's brute-force placement: slower, tighter.
    bool brute_force = false;
    // A group is repacked only if its images lose at least this fraction of
    // their texels (docs/uv.md#atlas-size).
    double min_gain = 0.1;
    gltf::ReadOptions read;
};

enum class Status : std::uint8_t {
    repacked,
    // The atlas wouldn't be smaller enough: left as it is.
    no_gain,
    // Left as it is, with a warning.
    skipped,
};

const char* statusName(Status status);

struct GroupImage {
    std::string source;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // Empty unless repacked.
    std::string output;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
};

// The materials that share images, repacked together.
struct Group {
    std::vector<std::string> materials;
    std::uint64_t primitives = 0;
    std::uint64_t charts = 0;
    Status status = Status::skipped;
    // The warning code, for skipped groups.
    std::string reason;
    std::vector<GroupImage> images;
    std::uint64_t texels_before = 0;
    // For a group left as it is for no gain, the atlas it would have had.
    std::uint64_t texels_after = 0;
    // Texels the charts cover, over all texels of the images: before and after.
    double occupancy_before = 0;
    double occupancy_after = 0;
    double seconds = 0;
};

struct Report {
    std::vector<Group> groups;
    double seconds = 0;
    std::vector<gltf::WrittenFile> files;
};

// Repacks the UV charts of each material group of `input` and bakes their
// images into the new layout, then writes the model as `<stem>.gltf`,
// `<stem>.bin` and its images in `directory` (docs/uv.md).
Report uv(const std::filesystem::path& input, const std::filesystem::path& directory, const std::string& stem,
          const Options& options, common::Workspace& workspace, common::Outputs& outputs,
          common::Diagnostics& diagnostics);

nlohmann::ordered_json toJson(const Report& report, nlohmann::ordered_json header, const common::Diagnostics& diagnostics);
void printText(std::ostream& out, const Report& report);

} // namespace meshtinker::uv
