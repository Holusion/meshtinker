#include "ktx.hpp"

#include "encode.hpp"

#include "meshtinker/image/image.hpp"
#include "meshtinker/texture/reencode.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>

namespace meshtinker::ktx {

namespace fs = std::filesystem;
using Json = gltf::Json;

const char* qualityName(Quality quality) {
    switch (quality) {
    case Quality::highest:
        return "Highest";
    case Quality::high:
        return "High";
    case Quality::medium:
        return "Medium";
    case Quality::low:
        return "Low";
    case Quality::thumb:
        return "Thumb";
    }
    return "";
}

const char* codecName(Codec codec) { return codec == Codec::etc1s ? "ETC1S" : "UASTC"; }

Settings settings(Quality quality) {
    // eCorpus's quality levels for ETC1S; UASTC level 1 at every quality
    // (docs/ktx.md#settings).
    Settings s;
    switch (quality) {
    case Quality::highest:
        s.etc1s_quality = 250;
        break;
    case Quality::high:
        s.etc1s_quality = 240;
        break;
    case Quality::medium:
        s.etc1s_quality = 220;
        break;
    case Quality::low:
        s.etc1s_quality = 180;
        break;
    case Quality::thumb:
        s.etc1s_quality = 128;
        break;
    }
    return s;
}

namespace {

using Clock = std::chrono::steady_clock;

// basisu's encoder, with the codec chosen from the image's use (docs/ktx.md#codec-per-slot).
class BasisuEncoder final : public texture::Encoder {
public:
    BasisuEncoder(unsigned threads, const Settings& settings) : encoder_(threads), settings_(settings) {}

    std::optional<std::vector<std::uint8_t>> encode(image::Image image, const texture::Request& request,
                                                    std::string& error) override {
        EncodeParams params;
        params.settings = settings_;
        params.codec = request.use.color ? ktx::Codec::etc1s : ktx::Codec::uastc;
        params.srgb = request.use.color;
        params.normal = request.use.normal;
        params.alpha = request.alpha;
        return encoder_.encode(std::move(image), params, error);
    }

private:
    ktx::Encoder encoder_;
    Settings settings_;
};

class KtxCodec final : public texture::Codec {
public:
    explicit KtxCodec(const Settings& settings) : settings_(settings) {}
    const char* command() const override { return "ktx"; }
    const char* library() const override { return "basisu"; }
    const char* extension() const override { return "KHR_texture_basisu"; }
    const char* mimeType() const override { return "image/ktx2"; }
    const char* fileExtension() const override { return ".ktx2"; }
    std::vector<const char*> keptExtensions() const override { return {}; }
    std::vector<const char*> replacedExtensions() const override { return {"EXT_texture_webp", "EXT_texture_avif"}; }
    // ETC1S and UASTC files are different files with different transfer functions.
    bool mixedUse() const override { return false; }
    // A KTX2 file stays compressed on the GPU, which a smaller PNG or JPEG doesn't.
    bool keepsSmallerSource() const override { return false; }
    // Block formats: sides are multiples of 4.
    std::uint32_t multiple() const override { return 4; }
    std::unique_ptr<texture::Encoder> encoder(unsigned threads) const override {
        return std::make_unique<BasisuEncoder>(threads, settings_);
    }

private:
    Settings settings_;
};

} // namespace

Report ktx(const fs::path& input, const fs::path& directory, const std::string& stem, const Options& options,
           common::Outputs& outputs, common::Diagnostics& diagnostics) {
    const Clock::time_point start = Clock::now();
    Report report;
    report.quality = options.quality;
    report.max_size = options.max_size;
    report.min_size = options.min_size;
    texture::Options texture_options;
    texture_options.max_size = options.max_size;
    texture_options.min_size = options.min_size;
    texture_options.threads = options.threads;
    texture_options.read = options.read;
    const KtxCodec codec(settings(options.quality));
    texture::Result result = texture::reencode(input, directory, stem, texture_options, codec, outputs, diagnostics);
    report.threads = result.threads;
    report.skipped = result.skipped;
    report.small = std::move(result.small);
    report.files = std::move(result.files);
    for (const texture::Encoded& image : result.encoded) {
        EncodedImage entry;
        entry.source = image.source;
        entry.name = image.name;
        entry.source_format = image.source_format;
        entry.width = image.width;
        entry.height = image.height;
        entry.codec = image.request.use.color ? Codec::etc1s : Codec::uastc;
        entry.srgb = image.request.use.color;
        entry.alpha = image.request.alpha;
        entry.image = image.image;
        entry.output = image.output;
        entry.output_width = image.output_width;
        entry.output_height = image.output_height;
        entry.bytes_before = image.bytes_before;
        entry.bytes_after = image.bytes_after;
        entry.seconds = image.seconds;
        report.encoded.push_back(std::move(entry));
    }
    report.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    return report;
}

} // namespace meshtinker::ktx
