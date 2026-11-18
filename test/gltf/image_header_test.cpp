#include "meshtinker/common/mapped_file.hpp"
#include "meshtinker/gltf/image_header.hpp"
#include "support/check.hpp"

#include <vector>

using namespace meshtinker;
using common::ByteView;
using gltf::ImageFormat;
using gltf::readImageHeader;

namespace {

std::optional<gltf::ImageHeader> fromFile(const std::string& path) {
    static std::vector<common::MappedFile> keep;
    keep.push_back(common::MappedFile::open(std::string(MESHTINKER_TEST_DATA) + "/" + path, common::Side::input, path));
    return readImageHeader(keep.back().bytes());
}

void checkHeader(const std::string& path, ImageFormat format, std::uint32_t width, std::uint32_t height) {
    const auto header = fromFile(path);
    CHECK(header.has_value());
    if (header) {
        CHECK_EQ(gltf::mimeType(header->format), std::string(gltf::mimeType(format)));
        CHECK_EQ(header->width, width);
        CHECK_EQ(header->height, height);
    }
}

} // namespace

TEST_CASE(reads_fixture_images) {
    checkHeader("alt-sources/fallback.png", ImageFormat::png, 16, 16);
    checkHeader("alt-sources/alternative.webp", ImageFormat::webp, 32, 32);
    checkHeader("alt-sources/ktx-only.ktx2", ImageFormat::ktx2, 64, 64);
    checkHeader("alt-sources/webp-only.webp", ImageFormat::webp, 8, 8);
}

TEST_CASE(reads_jpeg_after_other_segments) {
    // SOI, an APP0 segment of 16 bytes, a fill byte, then SOF2 (progressive): 3 x 2.
    const std::vector<std::uint8_t> jpeg = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1,
                                            0,    1,    0,    0,    0xFF, 0xFF, 0xC2, 0x00, 0x11, 8, 0, 2, 0, 3, 3};
    const auto header = readImageHeader(ByteView{jpeg.data(), jpeg.size()});
    CHECK(header.has_value());
    if (header) {
        CHECK(header->format == ImageFormat::jpeg);
        CHECK_EQ(header->width, 3u);
        CHECK_EQ(header->height, 2u);
    }
}

TEST_CASE(reads_lossy_and_extended_webp) {
    // VP8 key frame: frame tag, start code, 14-bit sizes 300 x 200.
    std::vector<std::uint8_t> lossy = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', ' ', 0, 0, 0, 0,
                                       0,   0,   0,   0x9D, 0x01, 0x2A, 0x2C, 0x01, 0xC8, 0x00};
    auto header = readImageHeader(ByteView{lossy.data(), lossy.size()});
    CHECK(header && header->width == 300 && header->height == 200);
    // VP8X canvas 4096 x 2 (stored minus one, 24 bits).
    std::vector<std::uint8_t> extended = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', 'X', 10, 0, 0, 0,
                                          0,   0,   0,   0,   0xFF, 0x0F, 0x00, 0x01, 0x00, 0x00};
    header = readImageHeader(ByteView{extended.data(), extended.size()});
    CHECK(header && header->width == 4096 && header->height == 2);
}

TEST_CASE(rejects_unknown_and_truncated_bytes) {
    const std::vector<std::uint8_t> text = {'h', 'e', 'l', 'l', 'o'};
    CHECK(!readImageHeader(ByteView{text.data(), text.size()}).has_value());
    const std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0};
    CHECK(!readImageHeader(ByteView{png.data(), png.size()}).has_value());
    CHECK(!readImageHeader(ByteView{}).has_value());
}
