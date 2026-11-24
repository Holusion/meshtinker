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
    checkHeader("avif/existing.avif", ImageFormat::avif, 32, 32);
}

// The codec comes from the KTX2 header: BasisLZ supercompression for ETC1S,
// the data format descriptor's color model for UASTC.
TEST_CASE(reads_ktx2_codec) {
    const auto codec = [](const std::string& path) {
        const auto header = fromFile(path);
        return header && header->ktx2_codec ? std::string(gltf::codecName(*header->ktx2_codec)) : std::string("none");
    };
    CHECK_EQ(codec("ktx2-sources/color.ktx2"), std::string("ETC1S"));
    CHECK_EQ(codec("ktx2-sources/alpha.ktx2"), std::string("ETC1S"));
    CHECK_EQ(codec("ktx2-sources/normal.ktx2"), std::string("UASTC"));
    CHECK_EQ(codec("alt-sources/fallback.png"), std::string("none"));
    // A KTX2 that stores a GPU format (vkFormat 37, R8G8B8A8_UNORM), and one
    // cut before its descriptor.
    std::vector<std::uint8_t> raw(160, 0);
    const char identifier[] = "\xABKTX 20\xBB\r\n\x1A\n";
    std::copy(identifier, identifier + 12, raw.begin());
    raw[12] = 37;
    raw[20] = 8;
    raw[24] = 8;
    auto header = readImageHeader(ByteView{raw.data(), raw.size()});
    CHECK(header && header->ktx2_codec == gltf::Ktx2Codec::other);
    raw[12] = 0;
    raw[48] = 200;
    header = readImageHeader(ByteView{raw.data(), raw.size()});
    CHECK(header && header->width == 8 && header->ktx2_codec == gltf::Ktx2Codec::other);
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

namespace {

// An ISOBMFF box: big-endian size, type, payload.
std::vector<std::uint8_t> box(const char* type, const std::vector<std::uint8_t>& payload) {
    const auto size = static_cast<std::uint32_t>(payload.size() + 8);
    std::vector<std::uint8_t> out = {static_cast<std::uint8_t>(size >> 24), static_cast<std::uint8_t>(size >> 16),
                                     static_cast<std::uint8_t>(size >> 8), static_cast<std::uint8_t>(size),
                                     static_cast<std::uint8_t>(type[0]), static_cast<std::uint8_t>(type[1]),
                                     static_cast<std::uint8_t>(type[2]), static_cast<std::uint8_t>(type[3])};
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<std::uint8_t> join(std::initializer_list<std::vector<std::uint8_t>> parts) {
    std::vector<std::uint8_t> out;
    for (const auto& part : parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

std::vector<std::uint8_t> ispe(std::uint8_t width_high, std::uint8_t width_low, std::uint8_t height_high,
                               std::uint8_t height_low) {
    return box("ispe", {0, 0, 0, 0, 0, 0, width_high, width_low, 0, 0, height_high, height_low});
}

} // namespace

// The size is the primary item's, through its `ipma` associations: here item
// 2, whose `ispe` is the second property, not the first.
TEST_CASE(reads_avif_primary_item_size) {
    const std::vector<std::uint8_t> ftyp = box("ftyp", {'m', 'i', 'f', '1', 0, 0, 0, 0, 'm', 'i', 'f', '1', 'a', 'v', 'i', 'f'});
    const std::vector<std::uint8_t> pitm = box("pitm", {0, 0, 0, 0, 0, 2});
    const std::vector<std::uint8_t> ipco = box("ipco", join({ispe(0, 16, 0, 16), ispe(0x01, 0x2C, 0, 200)}));
    // Version 0, flags 0: 16-bit item IDs, 7-bit property indices.
    const std::vector<std::uint8_t> ipma = box("ipma", {0, 0, 0, 0, 0, 0, 0, 2, 0, 1, 1, 0x81, 0, 2, 1, 0x82});
    const std::vector<std::uint8_t> meta = box("meta", join({{0, 0, 0, 0}, pitm, box("iprp", join({ipco, ipma}))}));
    const std::vector<std::uint8_t> file = join({ftyp, meta, box("mdat", {})});
    const auto header = readImageHeader(ByteView{file.data(), file.size()});
    CHECK(header.has_value());
    if (header) {
        CHECK(header->format == ImageFormat::avif);
        CHECK_EQ(header->width, 300u);
        CHECK_EQ(header->height, 200u);
    }
    // Without `meta`, the size can't be told.
    const std::vector<std::uint8_t> truncated = join({ftyp, box("mdat", {})});
    CHECK(!readImageHeader(ByteView{truncated.data(), truncated.size()}).has_value());
    // Another ISOBMFF brand isn't AVIF.
    const std::vector<std::uint8_t> heic = join({box("ftyp", {'h', 'e', 'i', 'c', 0, 0, 0, 0, 'm', 'i', 'f', '1'}), meta});
    CHECK(!readImageHeader(ByteView{heic.data(), heic.size()}).has_value());
}

TEST_CASE(rejects_unknown_and_truncated_bytes) {
    const std::vector<std::uint8_t> text = {'h', 'e', 'l', 'l', 'o'};
    CHECK(!readImageHeader(ByteView{text.data(), text.size()}).has_value());
    const std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0};
    CHECK(!readImageHeader(ByteView{png.data(), png.size()}).has_value());
    CHECK(!readImageHeader(ByteView{}).has_value());
}
