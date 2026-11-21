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

namespace meshtinker::ktx {

// eCorpus's quality names, which `optimize` passes through (docs/ktx.md#presets).
enum class Quality : std::uint8_t { highest, high, medium, low, thumb };

const char* qualityName(Quality quality);

enum class Codec : std::uint8_t { etc1s, uastc };

const char* codecName(Codec codec);

struct Options {
    Quality quality = Quality::high;
    // The longest side of an output image, 0 for no cap (docs/images.md#resizing).
    std::uint32_t max_size = 0;
    // Threads for basisu, 0 for every CPU the process may use.
    unsigned threads = 0;
    gltf::ReadOptions read;
};

// The basisu settings of a quality (docs/ktx.md#settings).
struct Settings {
    // ETC1S quality level, 1 to 255.
    int etc1s_quality = 0;
    // ETC1S compression level, 0 to 6.
    int etc1s_effort = 1;
    // UASTC level, 0 (fastest) to 4.
    int uastc_level = 1;
    int zstd_level = 18;
};

Settings settings(Quality quality);

// One image that was encoded.
struct EncodedImage {
    // The source image's index and file name.
    std::size_t source = 0;
    std::string name;
    std::string source_format;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    Codec codec = Codec::etc1s;
    bool srgb = false;
    bool alpha = false;
    // The output image's index and file name, and its size in texels.
    std::size_t image = 0;
    std::string output;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    std::uint64_t bytes_before = 0;
    std::uint64_t bytes_after = 0;
    double seconds = 0;
};

struct Report {
    Quality quality = Quality::high;
    std::uint32_t max_size = 0;
    unsigned threads = 1;
    std::vector<EncodedImage> encoded;
    // Images left as they are, each with its warning.
    std::uint64_t skipped = 0;
    double seconds = 0;
    std::vector<gltf::WrittenFile> files;
};

// Encodes the images of `input` as KTX2 and writes the model as `<stem>.gltf`,
// `<stem>.bin` and its images in `directory` (docs/ktx.md).
Report ktx(const std::filesystem::path& input, const std::filesystem::path& directory, const std::string& stem,
           const Options& options, common::Outputs& outputs, common::Diagnostics& diagnostics);

nlohmann::ordered_json toJson(const Report& report, nlohmann::ordered_json header, const common::Diagnostics& diagnostics);
void printText(std::ostream& out, const Report& report);

} // namespace meshtinker::ktx
