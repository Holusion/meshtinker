#include "meshtinker/image/image.hpp"

#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <fstream>
#include <iterator>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace meshtinker::image;

namespace {

Image filled(std::uint32_t width, std::uint32_t height, std::uint8_t r, std::uint8_t g, std::uint8_t b,
             std::uint8_t a) {
    Image image;
    image.width = width;
    image.height = height;
    for (std::size_t i = 0; i < image.texels(); ++i) {
        image.pixels.insert(image.pixels.end(), {r, g, b, a});
    }
    return image;
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc & 1u) != 0 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        }
    }
    return ~crc;
}

void appendBig(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
}

void appendChunk(std::vector<std::uint8_t>& out, const char* type, const std::vector<std::uint8_t>& data) {
    appendBig(out, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    appendBig(out, crc32(body.data(), body.size()));
}

// A 16-bit RGB PNG of one row, its deflate stream a single stored block.
std::vector<std::uint8_t> png16(const std::vector<std::uint16_t>& rgb) {
    std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<std::uint8_t> header;
    appendBig(header, static_cast<std::uint32_t>(rgb.size() / 3));
    appendBig(header, 1);
    header.insert(header.end(), {16, 2, 0, 0, 0});
    appendChunk(out, "IHDR", header);
    std::vector<std::uint8_t> raw = {0}; // filter type None
    for (const std::uint16_t value : rgb) {
        raw.push_back(static_cast<std::uint8_t>(value >> 8));
        raw.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    }
    std::vector<std::uint8_t> zlib = {0x78, 0x01, 0x01};
    const auto length = static_cast<std::uint16_t>(raw.size());
    const auto inverse = static_cast<std::uint16_t>(~length);
    zlib.insert(zlib.end(), {static_cast<std::uint8_t>(length & 0xFFu), static_cast<std::uint8_t>(length >> 8),
                             static_cast<std::uint8_t>(inverse & 0xFFu), static_cast<std::uint8_t>(inverse >> 8)});
    zlib.insert(zlib.end(), raw.begin(), raw.end());
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const std::uint8_t byte : raw) {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    appendBig(zlib, (b << 16) | a);
    appendChunk(out, "IDAT", zlib);
    appendChunk(out, "IEND", {});
    return out;
}

meshtinker::common::ByteView view(const std::vector<std::uint8_t>& bytes) { return {bytes.data(), bytes.size()}; }

} // namespace

TEST_CASE(png_round_trip) {
    Image image = filled(3, 2, 10, 20, 30, 255);
    image.pixels[7] = 128;
    std::string error;
    const std::vector<std::uint8_t> rgba = encodePng(image, 4);
    const std::optional<Image> decoded = decode(view(rgba), error);
    CHECK(decoded.has_value());
    CHECK(decoded && decoded->width == 3 && decoded->height == 2);
    CHECK(decoded && decoded->source_channels == 4);
    CHECK(decoded && decoded->pixels == image.pixels);

    // Three channels: alpha comes back opaque.
    const std::optional<Image> rgb = decode(view(encodePng(image, 3)), error);
    CHECK(rgb && rgb->source_channels == 3);
    CHECK(rgb && rgb->pixels[7] == 255 && rgb->pixels[4] == 10);
}

TEST_CASE(sixteen_bit_png_is_reduced_to_eight) {
    std::string error;
    const std::optional<Image> decoded = decode(view(png16({0xFFFF, 0x8000, 0x0000, 0x1234, 0xABCD, 0x00FF})), error);
    CHECK(decoded.has_value());
    CHECK(decoded && decoded->width == 2 && decoded->height == 1);
    // The high byte of each value, and opaque alpha.
    CHECK(decoded && decoded->pixels == (std::vector<std::uint8_t>{0xFF, 0x80, 0x00, 0xFF, 0x12, 0xAB, 0x00, 0xFF}));
}

TEST_CASE(undecodable_bytes_give_a_reason) {
    const std::vector<std::uint8_t> bytes = {'n', 'o', 't', ' ', 'a', 'n', ' ', 'i', 'm', 'a', 'g', 'e'};
    std::string error;
    CHECK(!decode(view(bytes), error).has_value());
    CHECK(!error.empty());
}

namespace {

// tools/fixtures/generate.mjs's pattern(): four quadrants of a hue, with a red
// gradient, opaque.
std::vector<std::uint8_t> pattern(int width, int height, int hue) {
    int colors[4][3] = {{230, 60, 60}, {60, 200, 60}, {60, 90, 230}, {240, 220, 60}};
    for (auto& color : colors) {
        for (int i = 0; i < 3; ++i) {
            color[i] = (color[i] + hue * (i + 1) * 37) % 256;
        }
    }
    std::vector<std::uint8_t> pixels;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int q = (2 * x < width ? 0 : 1) + (2 * y < height ? 0 : 2);
            const int g = static_cast<int>(std::lround(double(x) / std::max(1, width - 1) * 40));
            pixels.push_back(static_cast<std::uint8_t>(std::min(255, colors[q][0] + g)));
            pixels.push_back(static_cast<std::uint8_t>(colors[q][1]));
            pixels.push_back(static_cast<std::uint8_t>(colors[q][2]));
            pixels.push_back(255);
        }
    }
    return pixels;
}

std::vector<std::uint8_t> fixtureFile(const char* fixture, const char* name) {
    std::ifstream in(meshtinker::test::fixture(fixture).model.parent_path() / name, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

// Lossless WebP decodes to the generator's pixels, where the build decodes
// WebP; elsewhere it fails with a reason.
TEST_CASE(webp_decodes) {
    const std::vector<std::uint8_t> bytes = fixtureFile("alt-sources", "webp-only.webp");
    std::string error;
    const std::optional<Image> decoded = decode(view(bytes), error);
    if (!decodesWebp()) {
        CHECK(!decoded && !error.empty());
        return;
    }
    CHECK(decoded && decoded->width == 8 && decoded->height == 8 && decoded->source_channels == 3);
    CHECK(decoded && decoded->pixels == pattern(8, 8, 2));
}

// AVIF (quality 50) decodes close to the generator's pixels.
TEST_CASE(avif_decodes) {
    const std::vector<std::uint8_t> bytes = fixtureFile("avif", "existing.avif");
    std::string error;
    const std::optional<Image> decoded = decode(view(bytes), error);
    if (!decodesAvif()) {
        CHECK(!decoded && !error.empty());
        return;
    }
    CHECK(decoded && decoded->width == 32 && decoded->height == 32);
    if (decoded) {
        const std::vector<std::uint8_t> expected = pattern(32, 32, 6);
        double difference = 0;
        for (std::size_t i = 0; i < expected.size(); ++i) {
            difference += std::abs(int(decoded->pixels[i]) - int(expected[i]));
        }
        CHECK(difference / double(expected.size()) < 8);
    }
}

// KTX2 decodes through basisu's transcoder: ETC1S close to the generator's
// pattern, ETC1S with its alpha slice, UASTC (zstd) normals.
TEST_CASE(ktx2_decodes) {
    std::string error;
    const std::vector<std::uint8_t> color = fixtureFile("ktx2-sources", "color.ktx2");
    std::optional<Image> decoded = decode(view(color), error);
    CHECK(decoded && decoded->width == 64 && decoded->height == 64 && decoded->source_channels == 3);
    if (decoded) {
        const std::vector<std::uint8_t> expected = pattern(64, 64, 1);
        double difference = 0;
        for (std::size_t i = 0; i < expected.size(); ++i) {
            difference += std::abs(int(decoded->pixels[i]) - int(expected[i]));
        }
        CHECK(difference / double(expected.size()) < 8);
    }
    const std::vector<std::uint8_t> alpha = fixtureFile("ktx2-sources", "alpha.ktx2");
    decoded = decode(view(alpha), error);
    CHECK(decoded && decoded->source_channels == 4);
    if (decoded) {
        // The alpha gradient: 0 at the left, 255 at the right.
        CHECK(decoded->pixels[3] < 16);
        CHECK(decoded->pixels[63 * 4 + 3] > 239);
    }
    const std::vector<std::uint8_t> normal = fixtureFile("ktx2-sources", "normal.ktx2");
    decoded = decode(view(normal), error);
    CHECK(decoded && decoded->source_channels == 3);
    if (decoded) {
        // Near the center the normal points up the tangent space's Z.
        const std::size_t center = (32 * 64 + 32) * 4;
        CHECK(std::abs(int(decoded->pixels[center]) - 128) < 8);
        CHECK(std::abs(int(decoded->pixels[center + 1]) - 128) < 8);
        CHECK(decoded->pixels[center + 2] > 245);
    }
    // A truncated file fails with a reason.
    const std::vector<std::uint8_t> cut(color.begin(), color.begin() + 80);
    error.clear();
    CHECK(!decode(view(cut), error) && !error.empty());
}

// One channel writes red alone, as gray.
TEST_CASE(gray_png_keeps_red) {
    const Image image = filled(3, 2, 200, 10, 20, 255);
    std::string error;
    const std::vector<std::uint8_t> png = encodePng(image, 1);
    const std::optional<Image> decoded = decode(view(png), error);
    CHECK(decoded && decoded->source_channels == 1 && decoded->pixels[0] == 200 && decoded->pixels[1] == 200);
}

TEST_CASE(channel_analysis) {
    Image gray = filled(4, 4, 90, 90, 90, 255);
    Channels channels = analyze(gray);
    CHECK(!channels.alpha && !channels.color);
    gray.pixels[4 * 5 + 3] = 254;
    channels = analyze(gray);
    CHECK(channels.alpha && !channels.color);
    gray.pixels[4 * 9 + 1] = 91;
    channels = analyze(gray);
    CHECK(channels.alpha && channels.color);
}

TEST_CASE(fitted_sizes) {
    CHECK(fittedSize({4096, 2048}, 1024, 1) == (Size{1024, 512}));
    CHECK(fittedSize({1000, 3000}, 1024, 1) == (Size{341, 1024}));
    // Never enlarged; 0 means no cap.
    CHECK(fittedSize({300, 200}, 1024, 1) == (Size{300, 200}));
    CHECK(fittedSize({300, 200}, 0, 1) == (Size{300, 200}));
    // Multiples of 4 for KTX2, rounded up.
    CHECK(fittedSize({1001, 3}, 0, 4) == (Size{1004, 4}));
    CHECK(fittedSize({1000, 3000}, 1024, 4) == (Size{344, 1024}));
    CHECK(fittedSize({8192, 1}, 512, 4) == (Size{512, 4}));
}

TEST_CASE(resizing) {
    // A flat sRGB image stays flat.
    const Image flat = resize(filled(8, 8, 200, 100, 50, 255), {4, 4}, Content::srgb, false);
    CHECK(flat.width == 4 && flat.height == 4 && flat.pixels.size() == 64);
    bool same = true;
    for (std::size_t i = 0; i < flat.pixels.size(); i += 4) {
        same = same && flat.pixels[i] == 200 && flat.pixels[i + 1] == 100 && flat.pixels[i + 2] == 50 &&
               flat.pixels[i + 3] == 255;
    }
    CHECK(same);

    // Premultiplied alpha: a transparent texel's color doesn't bleed.
    Image half = filled(2, 1, 0, 0, 255, 255);
    half.pixels[0] = 255;
    half.pixels[2] = 0;
    half.pixels[3] = 0;
    const Image mixed = resize(half, {1, 1}, Content::srgb, true);
    CHECK(mixed.pixels[0] < 8 && mixed.pixels[2] > 247);

    // Normal maps come out unit length.
    Image normals = filled(2, 1, 255, 128, 128, 255);
    normals.pixels[4] = 128;
    normals.pixels[5] = 255;
    const Image averaged = resize(normals, {1, 1}, Content::normal, false);
    double length = 0;
    for (int c = 0; c < 3; ++c) {
        const double v = averaged.pixels[static_cast<std::size_t>(c)] / 127.5 - 1.0;
        length += v * v;
    }
    CHECK(std::abs(std::sqrt(length) - 1.0) < 0.02);
}

TEST_CASE(thread_count) { CHECK(threadCount() >= 1); }
