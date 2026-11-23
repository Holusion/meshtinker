#include "commands/avif/avif.hpp"

#include "meshtinker/common/diagnostics.hpp"
#include "meshtinker/common/outputs.hpp"
#include "meshtinker/gltf/image_header.hpp"
#include "meshtinker/image/image.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <avif/avif.h>

#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace meshtinker;
using Json = nlohmann::ordered_json;

namespace {

std::vector<std::uint8_t> readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct Run {
    avif::Report report;
    common::Diagnostics diagnostics;
    Json json;
    fs::path directory;
};

Run encode(const char* fixture, const char* name, const avif::Options& options = {}) {
    Run run;
    run.directory = test::workDirectory(std::string("avif/") + name);
    common::Outputs outputs;
    avif::Options trusted = options;
    trusted.read.trusted = true;
    run.report = avif::avif(test::fixture(fixture).model, run.directory, name, trusted, outputs, run.diagnostics);
    outputs.commit();
    std::ifstream in(run.directory / (std::string(name) + ".gltf"));
    run.json = Json::parse(in);
    return run;
}

// An AVIF file as libavif decodes it.
struct Decoded {
    bool ok = false;
    avifPixelFormat format = AVIF_PIXEL_FORMAT_NONE;
    bool alpha = false;
    avifColorPrimaries primaries = AVIF_COLOR_PRIMARIES_UNSPECIFIED;
    avifTransferCharacteristics transfer = AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED;
    avifMatrixCoefficients matrix = AVIF_MATRIX_COEFFICIENTS_UNSPECIFIED;
    avifRange range = AVIF_RANGE_LIMITED;
    image::Image rgba;
};

Decoded decodeAvif(const fs::path& path) {
    const std::vector<std::uint8_t> bytes = readFile(path);
    Decoded decoded;
    avifDecoder* decoder = avifDecoderCreate();
    avifImage* image = avifImageCreateEmpty();
    if (avifDecoderReadMemory(decoder, image, bytes.data(), bytes.size()) == AVIF_RESULT_OK) {
        decoded.format = image->yuvFormat;
        decoded.alpha = image->alphaPlane != nullptr;
        decoded.primaries = image->colorPrimaries;
        decoded.transfer = image->transferCharacteristics;
        decoded.matrix = image->matrixCoefficients;
        decoded.range = image->yuvRange;
        avifRGBImage rgb;
        avifRGBImageSetDefaults(&rgb, image);
        rgb.format = AVIF_RGB_FORMAT_RGBA;
        rgb.depth = 8;
        decoded.rgba.width = image->width;
        decoded.rgba.height = image->height;
        decoded.rgba.pixels.resize(decoded.rgba.texels() * 4);
        rgb.pixels = decoded.rgba.pixels.data();
        rgb.rowBytes = image->width * 4;
        decoded.ok = avifImageYUVToRGB(image, &rgb) == AVIF_RESULT_OK;
    }
    avifImageDestroy(image);
    avifDecoderDestroy(decoder);
    return decoded;
}

image::Image decodeSource(const fs::path& path) {
    const std::vector<std::uint8_t> bytes = readFile(path);
    std::string error;
    std::optional<image::Image> decoded = image::decode({bytes.data(), bytes.size()}, error);
    return decoded ? *decoded : image::Image{};
}

// PSNR over RGB, and alpha when `alpha`.
double psnr(const image::Image& a, const image::Image& b, bool alpha) {
    if (a.width != b.width || a.height != b.height || a.pixels.empty()) {
        return 0;
    }
    double sum = 0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        if (i % 4 == 3 && !alpha) {
            continue;
        }
        const double d = static_cast<double>(a.pixels[i]) - static_cast<double>(b.pixels[i]);
        sum += d * d;
        ++count;
    }
    const double mse = sum / static_cast<double>(count);
    return mse == 0 ? 99 : 10 * std::log10(255.0 * 255.0 / mse);
}

const avif::EncodedImage* find(const avif::Report& report, const std::string& source) {
    for (const avif::EncodedImage& image : report.encoded) {
        if (image.image.name == source) {
            return &image;
        }
    }
    return nullptr;
}

std::uint64_t encodedBytes(const avif::Report& report) {
    std::uint64_t total = 0;
    for (const avif::EncodedImage& image : report.encoded) {
        total += image.image.bytes_after;
    }
    return total;
}

} // namespace

// Each image's pixel format follows its slots, as expected.json lists them:
// color 4:2:0, data and color-and-data 4:4:4, gray 4:0:0. Only the BLEND
// material's image keeps alpha. Every file has the sRGB description, and
// decodes close to its source.
TEST_CASE(slots_decide_pixel_format_and_alpha) {
    const test::Fixture& fixture = test::fixture("avif");
    Run run = encode("avif", "slots");
    const Json& expected = fixture.expected["avif"];
    CHECK_EQ(run.report.encoded.size(), expected["encoded"].size());
    for (const auto& [source, format] : expected["encoded"].items()) {
        const avif::EncodedImage* image = find(run.report, source);
        CHECK(image != nullptr);
        if (image == nullptr) {
            continue;
        }
        CHECK_EQ(std::string(avif::subsamplingName(image->subsampling)), format.get<std::string>());
        const bool alpha = std::find(expected["alpha"].begin(), expected["alpha"].end(), source) != expected["alpha"].end();
        CHECK_EQ(image->image.request.alpha, alpha);
        CHECK(image->image.bytes_after < image->image.bytes_before);

        const Decoded decoded = decodeAvif(run.directory / image->image.output);
        CHECK(decoded.ok);
        const avifPixelFormat formats[] = {AVIF_PIXEL_FORMAT_YUV420, AVIF_PIXEL_FORMAT_YUV444, AVIF_PIXEL_FORMAT_YUV400};
        CHECK_EQ(decoded.format, formats[static_cast<int>(image->subsampling)]);
        CHECK_EQ(decoded.alpha, alpha);
        CHECK_EQ(decoded.primaries, AVIF_COLOR_PRIMARIES_BT709);
        CHECK_EQ(decoded.transfer, AVIF_TRANSFER_CHARACTERISTICS_SRGB);
        CHECK_EQ(decoded.matrix, AVIF_MATRIX_COEFFICIENTS_BT601);
        CHECK_EQ(decoded.range, AVIF_RANGE_FULL);
        // Noisy images at quality High, whose noise AVIF smooths: above 20 dB,
        // below lossless.
        const image::Image source_pixels = decodeSource(fixture.model.parent_path() / source);
        const double quality = psnr(decoded.rgba, source_pixels, alpha);
        CHECK(quality > 20 && quality < 60);

        const auto header = gltf::readImageHeader({readFile(run.directory / image->image.output).data(),
                                                   readFile(run.directory / image->image.output).size()});
        CHECK(header && header->format == gltf::ImageFormat::avif && header->width == 48 && header->height == 48);
    }
    CHECK(run.json["extensionsRequired"].dump().find("EXT_texture_avif") != std::string::npos);
}

// The 4×4 PNG is smaller than any AVIF of it: it stays, and its texture
// keeps its core source.
TEST_CASE(sources_smaller_than_their_avif_stay) {
    const test::Fixture& fixture = test::fixture("avif");
    Run run = encode("avif", "not-smaller");
    std::vector<std::string> expected;
    for (const auto& name : fixture.expected["avif"]["notSmaller"]) {
        expected.push_back(name.get<std::string>());
    }
    // The 8×8 WebP too, where the build decodes WebP.
    if (image::decodesWebp()) {
        expected.emplace_back("webp-only.webp");
    }
    CHECK(run.report.not_smaller == expected);
    CHECK(fs::exists(run.directory / "tiny.png"));
    CHECK(!fs::exists(run.directory / "tiny.avif"));
    bool kept = false;
    for (const Json& texture : run.json["textures"]) {
        if (texture.contains("source")) {
            const Json& image = run.json["images"][texture["source"].get<std::size_t>()];
            kept = kept || image["uri"] == "tiny.png";
        }
    }
    CHECK(kept);
}

// WebP alternatives go with the source they were an alternative to; KTX2 and
// AVIF textures stay as they are; a WebP-only texture is encoded where the
// build decodes WebP (and kept here, its AVIF being larger), and stays with a
// warning elsewhere.
TEST_CASE(other_sources_are_removed_and_unsupported_ones_kept) {
    Run run = encode("avif", "alt-sources");
    const bool webp = image::decodesWebp();
    CHECK_EQ(run.report.skipped, std::uint64_t{webp ? 0U : 1U});
    const Json diagnostics = run.diagnostics.toJson();
    bool unsupported = false;
    for (const Json& diagnostic : diagnostics) {
        unsupported = unsupported || diagnostic["code"] == "image-format-unsupported";
    }
    CHECK(unsupported != webp);
    CHECK(!fs::exists(run.directory / "alternative.webp"));
    CHECK(!fs::exists(run.directory / "fallback.png"));
    CHECK(fs::exists(run.directory / "fallback.avif"));
    CHECK(fs::exists(run.directory / "ktx-only.ktx2"));
    CHECK(fs::exists(run.directory / "webp-only.webp"));
    CHECK(fs::exists(run.directory / "existing.avif"));
    std::map<std::string, int> sources;
    for (const Json& texture : run.json["textures"]) {
        if (texture.contains("extensions")) {
            for (const auto& [name, extension] : texture["extensions"].items()) {
                ++sources[name];
            }
        }
        CHECK(!(texture.contains("source") && texture.contains("extensions") &&
                texture["extensions"].contains("EXT_texture_avif")));
    }
    CHECK_EQ(sources["KHR_texture_basisu"], 1);
    CHECK_EQ(sources["EXT_texture_webp"], 1);
    // The textures of the 7 encoded images (orm.png and mixed.png have two
    // each), and existing.avif's.
    CHECK_EQ(sources["EXT_texture_avif"], 10);
}

// Lower presets write smaller files and decode further from the source.
TEST_CASE(presets_trade_size_for_quality) {
    avif::Options thumb;
    thumb.quality = avif::Quality::thumb;
    avif::Options highest;
    highest.quality = avif::Quality::highest;
    Run low = encode("avif", "thumb", thumb);
    Run high = encode("avif", "highest", highest);
    CHECK(encodedBytes(low.report) < encodedBytes(high.report));
    const fs::path source = test::fixture("avif").model.parent_path() / "color.png";
    const image::Image pixels = decodeSource(source);
    CHECK(psnr(decodeAvif(low.directory / "color.avif").rgba, pixels, false) <
          psnr(decodeAvif(high.directory / "color.avif").rgba, pixels, false));
    CHECK_EQ(avif::qualityLevel(avif::Quality::thumb), 30);
    CHECK_EQ(avif::qualityLevel(avif::Quality::highest), 80);
}

// --min-size leaves small images as they are.
TEST_CASE(min_size_leaves_small_images) {
    avif::Options options;
    options.min_size = 32;
    Run run = encode("avif", "min-size", options);
    CHECK(find(run.report, "normal.png") != nullptr);
    CHECK(find(run.report, "tiny.png") == nullptr);
    CHECK(!run.report.small.empty());
    CHECK(fs::exists(run.directory / "tiny.png"));
}

// --max-size scales down, without rounding sizes to a block multiple.
TEST_CASE(max_size_scales_down) {
    avif::Options options;
    options.max_size = 20;
    Run run = encode("avif", "max-size", options);
    const avif::EncodedImage* normal = find(run.report, "normal.png");
    CHECK(normal != nullptr && normal->image.output_width == 20 && normal->image.output_height == 20);
    const Decoded decoded = decodeAvif(run.directory / "normal.avif");
    CHECK(decoded.ok && decoded.rgba.width == 20 && decoded.rgba.height == 20);
}
