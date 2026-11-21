#include "meshtinker/image/image.hpp"

#include "support/check.hpp"

#include <cmath>
#include <cstdint>
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
