#include "commands/ktx/ktx.hpp"

#include "meshtinker/common/diagnostics.hpp"
#include "meshtinker/common/outputs.hpp"
#include "meshtinker/image/image.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <transcoder/basisu_transcoder.h>

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
    ktx::Report report;
    common::Diagnostics diagnostics;
    Json json;
    fs::path directory;
};

Run encode(const char* fixture, const char* name, const ktx::Options& options = {}) {
    Run run;
    run.directory = test::workDirectory(std::string("ktx/") + name);
    common::Outputs outputs;
    ktx::Options trusted = options;
    trusted.read.trusted = true;
    run.report = ktx::ktx(test::fixture(fixture).model, run.directory, name, trusted, outputs, run.diagnostics);
    outputs.commit();
    std::ifstream in(run.directory / (std::string(name) + ".gltf"));
    run.json = Json::parse(in);
    return run;
}

struct Decoded {
    bool ok = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t levels = 0;
    bool etc1s = false;
    bool srgb = false;
    bool alpha = false;
    basist::ktx2_df_color_primaries primaries = basist::KTX2_DF_PRIMARIES_UNSPECIFIED;
    image::Image level0;
};

Decoded decodeKtx2(const fs::path& path) {
    static const bool initialized = (basist::basisu_transcoder_init(), true);
    static_cast<void>(initialized);
    const std::vector<std::uint8_t> bytes = readFile(path);
    basist::ktx2_transcoder transcoder;
    Decoded decoded;
    if (!transcoder.init(bytes.data(), static_cast<std::uint32_t>(bytes.size())) || !transcoder.start_transcoding()) {
        return decoded;
    }
    decoded.width = transcoder.get_width();
    decoded.height = transcoder.get_height();
    decoded.levels = transcoder.get_levels();
    decoded.etc1s = transcoder.is_etc1s();
    decoded.srgb = transcoder.is_srgb();
    decoded.alpha = transcoder.get_has_alpha() != 0;
    decoded.primaries = transcoder.get_dfd_color_primaries();
    decoded.level0.width = decoded.width;
    decoded.level0.height = decoded.height;
    decoded.level0.pixels.resize(decoded.level0.texels() * 4);
    decoded.ok = transcoder.transcode_image_level(0, 0, 0, decoded.level0.pixels.data(),
                                                  static_cast<std::uint32_t>(decoded.level0.texels()),
                                                  basist::transcoder_texture_format::cTFRGBA32);
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

const ktx::EncodedImage* find(const ktx::Report& report, const std::string& source) {
    for (const ktx::EncodedImage& image : report.encoded) {
        if (image.name == source) {
            return &image;
        }
    }
    return nullptr;
}

std::uint32_t fullChain(std::uint32_t width, std::uint32_t height) {
    std::uint32_t levels = 1;
    for (std::uint32_t side = std::max(width, height); side > 1; side /= 2) {
        ++levels;
    }
    return levels;
}

} // namespace

TEST_CASE(slots_decide_codec_transfer_function_and_alpha) {
    Run run = encode("textures", "textures");
    CHECK_EQ(run.report.encoded.size(), std::size_t{9});
    CHECK_EQ(run.report.skipped, std::uint64_t{0});
    CHECK(run.diagnostics.toJson().empty());

    struct Case {
        const char* source;
        ktx::Codec codec;
        bool alpha;
    };
    const Case cases[] = {
        {"alpha.png", ktx::Codec::etc1s, true},
        {"opaque.png", ktx::Codec::etc1s, false},        // alpha all 255
        {"ignored-alpha.png", ktx::Codec::etc1s, false}, // OPAQUE: alpha isn't sampled
        {"emissive.jpg", ktx::Codec::etc1s, false},
        {"normal.png", ktx::Codec::uastc, false},
        {"orm.png", ktx::Codec::uastc, false},
        {"occlusion.png", ktx::Codec::uastc, false},
        {"shared.png", ktx::Codec::etc1s, false},
        {"deep.png", ktx::Codec::etc1s, false},
    };
    const fs::path source_dir = test::fixture("textures").model.parent_path();
    for (const Case& c : cases) {
        const ktx::EncodedImage* image = find(run.report, c.source);
        CHECK(image != nullptr);
        if (image == nullptr) {
            continue;
        }
        CHECK(image->codec == c.codec);
        CHECK_EQ(image->srgb, c.codec == ktx::Codec::etc1s);
        CHECK_EQ(image->alpha, c.alpha);
        CHECK_EQ(image->output, fs::path(c.source).replace_extension(".ktx2").string());
        const Decoded decoded = decodeKtx2(run.directory / image->output);
        CHECK(decoded.ok);
        CHECK_EQ(decoded.etc1s, c.codec == ktx::Codec::etc1s);
        CHECK_EQ(decoded.srgb, image->srgb);
        CHECK_EQ(decoded.alpha, c.alpha);
        // KHR_texture_basisu: BT.709 with sRGB, unspecified primaries with linear.
        CHECK(decoded.primaries == (image->srgb ? basist::KTX2_DF_PRIMARIES_BT709 : basist::KTX2_DF_PRIMARIES_UNSPECIFIED));
        CHECK(decoded.width % 4 == 0 && decoded.height % 4 == 0);
        CHECK_EQ(decoded.levels, fullChain(decoded.width, decoded.height));

        image::Image source = decodeSource(source_dir / c.source);
        if (source.width != decoded.width || source.height != decoded.height) {
            source = image::resize(source, {decoded.width, decoded.height}, image::Content::srgb, c.alpha);
        }
        // ETC1S on 16×16 patterns with hard quadrant edges, some resized: the
        // threshold catches broken output (wrong channels, transfer function or
        // orientation), not small losses.
        const double quality = psnr(source, decoded.level0, c.alpha);
        if (quality < 25) {
            test::fail(std::string(c.source) + ": PSNR " + std::to_string(quality), __FILE__, __LINE__);
        }
    }

    // A size that isn't a multiple of 4 is rounded up.
    const ktx::EncodedImage* odd = find(run.report, "alpha.png");
    CHECK(odd != nullptr && odd->width == 30 && odd->height == 18 && odd->output_width == 32 && odd->output_height == 20);

    // Every texture has the KTX2 source only; one image serves both shared textures.
    const Json& textures = run.json["textures"];
    CHECK_EQ(textures.size(), std::size_t{10});
    for (const Json& texture : textures) {
        CHECK(!texture.contains("source"));
        CHECK(texture["extensions"]["KHR_texture_basisu"]["source"].is_number_unsigned());
    }
    CHECK_EQ(textures[7]["extensions"]["KHR_texture_basisu"]["source"], textures[9]["extensions"]["KHR_texture_basisu"]["source"]);
    CHECK_EQ(run.json["images"].size(), std::size_t{9});
    for (const Json& image : run.json["images"]) {
        CHECK_EQ(image["mimeType"].get<std::string>(), std::string("image/ktx2"));
    }
    CHECK(run.json["extensionsUsed"] == Json::array({"KHR_texture_basisu"}));
    CHECK(run.json["extensionsRequired"] == Json::array({"KHR_texture_basisu"}));
    for (const auto& entry : fs::directory_iterator(run.directory)) {
        const std::string extension = entry.path().extension().string();
        CHECK(extension == ".ktx2" || extension == ".gltf" || extension == ".bin");
    }
}

TEST_CASE(max_size_scales_down) {
    ktx::Options options;
    options.max_size = 8;
    options.quality = ktx::Quality::thumb;
    Run run = encode("textures", "max-size", options);
    const ktx::EncodedImage* odd = find(run.report, "alpha.png");
    // 30×18 to 8×5, then 8×8 for multiples of 4.
    CHECK(odd != nullptr && odd->output_width == 8 && odd->output_height == 8);
    const ktx::EncodedImage* square = find(run.report, "normal.png");
    CHECK(square != nullptr && square->output_width == 8 && square->output_height == 8);
    const Decoded decoded = decodeKtx2(run.directory / "normal.ktx2");
    CHECK(decoded.ok && decoded.width == 8 && decoded.levels == 4);
}

TEST_CASE(other_sources_are_removed_and_unsupported_ones_kept) {
    Run run = encode("alt-sources", "alt-sources");
    // fallback.png is encoded; ktx-only.ktx2 stays; webp-only.webp is left with a warning.
    CHECK_EQ(run.report.encoded.size(), std::size_t{1});
    CHECK_EQ(run.report.skipped, std::uint64_t{1});
    const Json diagnostics = run.diagnostics.toJson();
    CHECK(diagnostics.size() == 1 && diagnostics[0]["code"] == "image-format-unsupported");
    const Json& textures = run.json["textures"];
    CHECK(!textures[0].contains("source"));
    CHECK(!textures[0]["extensions"].contains("EXT_texture_webp"));
    CHECK(textures[0]["extensions"].contains("KHR_texture_basisu"));
    CHECK(textures[2]["extensions"].contains("EXT_texture_webp"));
    // alternative.webp went with the source it was an alternative to.
    for (const Json& image : run.json["images"]) {
        CHECK(image["uri"] != "alternative.webp" && image["uri"] != "fallback.png");
    }
    CHECK(fs::exists(run.directory / "ktx-only.ktx2"));
    CHECK(fs::exists(run.directory / "webp-only.webp"));
    CHECK(fs::exists(run.directory / "fallback.ktx2"));
}

TEST_CASE(color_and_data_on_one_image_is_left_as_it_is) {
    Run run = encode("occupancy", "occupancy");
    CHECK_EQ(run.report.encoded.size(), std::size_t{10});
    CHECK_EQ(run.report.skipped, std::uint64_t{1});
    const Json diagnostics = run.diagnostics.toJson();
    CHECK(diagnostics.size() == 1 && diagnostics[0]["code"] == "image-color-space-conflict");
    CHECK(fs::exists(run.directory / "shared.png"));
    const ktx::EncodedImage* clearcoat = find(run.report, "slot-clearcoat.png");
    CHECK(clearcoat != nullptr && clearcoat->codec == ktx::Codec::uastc && !clearcoat->srgb);
    const ktx::EncodedImage* emissive = find(run.report, "slot-emissive.png");
    CHECK(emissive != nullptr && emissive->codec == ktx::Codec::etc1s && emissive->srgb);
}
