#include "avif.hpp"

#include "encode.hpp"

#include <chrono>
#include <memory>

namespace meshtinker::avif {

namespace fs = std::filesystem;

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

int qualityLevel(Quality quality) {
    // From the rendered comparison's close-ups (docs/avif.md#quality-and-effort).
    switch (quality) {
    case Quality::highest:
        return 80;
    case Quality::high:
        return 65;
    case Quality::medium:
        return 50;
    case Quality::low:
        return 40;
    case Quality::thumb:
        return 30;
    }
    return 65;
}

const char* subsamplingName(Subsampling subsampling) {
    switch (subsampling) {
    case Subsampling::yuv420:
        return "4:2:0";
    case Subsampling::yuv444:
        return "4:4:4";
    case Subsampling::yuv400:
        return "4:0:0";
    }
    return "";
}

Subsampling subsampling(const texture::Request& request, int source_channels) {
    // Gray sources keep only their luma; color keeps chroma at half
    // resolution, as photos do; normal and other data maps keep it all, since
    // each channel holds its own value (docs/avif.md#per-slot-settings).
    if (source_channels <= 2) {
        return Subsampling::yuv400;
    }
    return request.use.data || request.use.normal ? Subsampling::yuv444 : Subsampling::yuv420;
}

namespace {

using Clock = std::chrono::steady_clock;

class AvifCodec final : public texture::Codec {
public:
    AvifCodec(int quality, int effort) : quality_(quality), effort_(effort) {}
    const char* command() const override { return "avif"; }
    const char* library() const override { return "libavif"; }
    const char* extension() const override { return "EXT_texture_avif"; }
    const char* mimeType() const override { return "image/avif"; }
    const char* fileExtension() const override { return ".avif"; }
    // KTX2 textures stay: they are block-compressed on the GPU, which AVIF isn't.
    std::vector<const char*> keptExtensions() const override { return {"KHR_texture_basisu"}; }
    std::vector<const char*> replacedExtensions() const override { return {"EXT_texture_webp"}; }
    // The transfer function is the slot's, not the file's: one file serves both.
    bool mixedUse() const override { return true; }
    // AVIF and the source decode to the same texture: the smaller file wins.
    // Tiny images are smaller as PNG, under AVIF's container (docs/avif.md#when-the-source-stays).
    bool keepsSmallerSource() const override { return true; }
    std::uint32_t multiple() const override { return 1; }
    std::unique_ptr<texture::Encoder> encoder(unsigned threads) const override {
        return std::make_unique<Encoder>(threads, quality_, effort_);
    }

private:
    int quality_;
    int effort_;
};

} // namespace

Report avif(const fs::path& input, const fs::path& directory, const std::string& stem, const Options& options,
            common::Outputs& outputs, common::Diagnostics& diagnostics) {
    const Clock::time_point start = Clock::now();
    Report report;
    report.quality = options.quality;
    report.effort = options.effort;
    report.max_size = options.max_size;
    report.min_size = options.min_size;
    texture::Options texture_options;
    texture_options.max_size = options.max_size;
    texture_options.min_size = options.min_size;
    texture_options.threads = options.threads;
    texture_options.read = options.read;
    const AvifCodec codec(qualityLevel(options.quality), options.effort);
    texture::Result result = texture::reencode(input, directory, stem, texture_options, codec, outputs, diagnostics);
    report.threads = result.threads;
    report.skipped = result.skipped;
    report.small = std::move(result.small);
    report.not_smaller = std::move(result.not_smaller);
    report.files = std::move(result.files);
    for (texture::Encoded& image : result.encoded) {
        const Subsampling format = subsampling(image.request, image.source_channels);
        report.encoded.push_back({std::move(image), format});
    }
    report.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    return report;
}

} // namespace meshtinker::avif
