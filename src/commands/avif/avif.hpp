#pragma once

#include "meshtinker/common/diagnostics.hpp"
#include "meshtinker/common/outputs.hpp"
#include "meshtinker/gltf/document.hpp"
#include "meshtinker/gltf/writer.hpp"
#include "meshtinker/texture/reencode.hpp"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace meshtinker::avif {

// eCorpus's quality names, as `ktx` takes them (docs/avif.md#quality-and-effort).
enum class Quality : std::uint8_t { highest, high, medium, low, thumb };

const char* qualityName(Quality quality);

// libavif's quality, 0 to 100, for a preset.
int qualityLevel(Quality quality);

// The default effort: sharp's and vips's scale, 0 (fastest) to 9
// (docs/avif.md#quality-and-effort).
constexpr int default_effort = 3;

struct Options {
    Quality quality = Quality::high;
    int effort = default_effort;
    // The longest side of an output image, 0 for no cap (docs/images.md#resizing).
    std::uint32_t max_size = 0;
    // Threads, 0 for every CPU the process may use.
    unsigned threads = 0;
    gltf::ReadOptions read;
};

// How an image's color planes are stored (docs/avif.md#per-slot-settings).
enum class Subsampling : std::uint8_t { yuv420, yuv444, yuv400 };

const char* subsamplingName(Subsampling subsampling);

// The pixel format of an image used as `request` asks, `source_channels` as decoded.
Subsampling subsampling(const texture::Request& request, int source_channels);

struct EncodedImage {
    texture::Encoded image;
    Subsampling subsampling = Subsampling::yuv420;
};

struct Report {
    Quality quality = Quality::high;
    int effort = default_effort;
    std::uint32_t max_size = 0;
    unsigned threads = 1;
    std::vector<EncodedImage> encoded;
    // Images left as they are, each with its warning.
    std::uint64_t skipped = 0;
    // Images kept because their AVIF wasn't smaller.
    std::vector<std::string> not_smaller;
    double seconds = 0;
    std::vector<gltf::WrittenFile> files;
};

// Encodes the images of `input` as AVIF and writes the model as `<stem>.gltf`,
// `<stem>.bin` and its images in `directory` (docs/avif.md).
Report avif(const std::filesystem::path& input, const std::filesystem::path& directory, const std::string& stem,
            const Options& options, common::Outputs& outputs, common::Diagnostics& diagnostics);

nlohmann::ordered_json toJson(const Report& report, nlohmann::ordered_json header, const common::Diagnostics& diagnostics);
void printText(std::ostream& out, const Report& report);

} // namespace meshtinker::avif
