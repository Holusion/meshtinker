#include "meshtinker/gltf/image_header.hpp"

#include <cstring>

namespace meshtinker::gltf {
namespace {

using common::ByteView;

std::uint32_t bigEndian16(const std::uint8_t* p) { return (std::uint32_t{p[0]} << 8) | p[1]; }
std::uint32_t bigEndian32(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}
std::uint32_t littleEndian16(const std::uint8_t* p) { return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8); }
std::uint32_t littleEndian24(const std::uint8_t* p) { return littleEndian16(p) | (std::uint32_t{p[2]} << 16); }
std::uint32_t littleEndian32(const std::uint8_t* p) { return littleEndian24(p) | (std::uint32_t{p[3]} << 24); }

bool startsWith(ByteView bytes, std::size_t offset, const char* text, std::size_t length) {
    return bytes.contains(offset, length) && std::memcmp(bytes.data + offset, text, length) == 0;
}

// PNG: the signature, then IHDR with big-endian width and height.
std::optional<ImageHeader> readPng(ByteView bytes) {
    if (!startsWith(bytes, 0, "\x89PNG\r\n\x1a\n", 8) || !startsWith(bytes, 12, "IHDR", 4) || !bytes.contains(16, 8)) {
        return std::nullopt;
    }
    return ImageHeader{ImageFormat::png, bigEndian32(bytes.data + 16), bigEndian32(bytes.data + 20)};
}

// JPEG: walks the markers to the first start-of-frame segment (SOF0 to SOF15,
// except DHT, JPG and DAC, which share the range).
std::optional<ImageHeader> readJpeg(ByteView bytes) {
    if (!bytes.contains(0, 3) || bytes.data[0] != 0xFF || bytes.data[1] != 0xD8) {
        return std::nullopt;
    }
    std::size_t offset = 2;
    while (bytes.contains(offset, 2)) {
        if (bytes.data[offset] != 0xFF) {
            return std::nullopt;
        }
        const std::uint8_t marker = bytes.data[offset + 1];
        if (marker == 0xFF) {
            ++offset; // fill byte
            continue;
        }
        offset += 2;
        // Markers without a length: TEM, RST0-7, SOI, EOI.
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD9)) {
            continue;
        }
        if (!bytes.contains(offset, 2)) {
            return std::nullopt;
        }
        const std::uint32_t length = bigEndian16(bytes.data + offset);
        const bool start_of_frame = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (start_of_frame) {
            if (length < 7 || !bytes.contains(offset, 7)) {
                return std::nullopt;
            }
            return ImageHeader{ImageFormat::jpeg, bigEndian16(bytes.data + offset + 5), bigEndian16(bytes.data + offset + 3)};
        }
        if (length < 2) {
            return std::nullopt;
        }
        offset += length;
    }
    return std::nullopt;
}

// WebP: a RIFF container whose first chunk is VP8 (lossy), VP8L (lossless) or
// VP8X (extended, which states the canvas size).
std::optional<ImageHeader> readWebp(ByteView bytes) {
    if (!startsWith(bytes, 0, "RIFF", 4) || !startsWith(bytes, 8, "WEBP", 4) || !bytes.contains(12, 8)) {
        return std::nullopt;
    }
    const std::uint8_t* chunk = bytes.data + 20;
    if (startsWith(bytes, 12, "VP8 ", 4)) {
        // Frame tag (3 bytes), start code 9D 01 2A, then 14-bit width and height.
        if (!bytes.contains(20, 10) || chunk[3] != 0x9D || chunk[4] != 0x01 || chunk[5] != 0x2A) {
            return std::nullopt;
        }
        return ImageHeader{ImageFormat::webp, littleEndian16(chunk + 6) & 0x3FFF, littleEndian16(chunk + 8) & 0x3FFF};
    }
    if (startsWith(bytes, 12, "VP8L", 4)) {
        // Signature 0x2F, then width - 1 and height - 1 in 14 bits each.
        if (!bytes.contains(20, 5) || chunk[0] != 0x2F) {
            return std::nullopt;
        }
        const std::uint32_t bits = littleEndian32(chunk + 1);
        return ImageHeader{ImageFormat::webp, (bits & 0x3FFF) + 1, ((bits >> 14) & 0x3FFF) + 1};
    }
    if (startsWith(bytes, 12, "VP8X", 4)) {
        // Flags and reserved bytes, then canvas width - 1 and height - 1 in 24 bits each.
        if (!bytes.contains(20, 10)) {
            return std::nullopt;
        }
        return ImageHeader{ImageFormat::webp, littleEndian24(chunk + 4) + 1, littleEndian24(chunk + 7) + 1};
    }
    return std::nullopt;
}

// KTX2: the identifier, then vkFormat, typeSize, pixelWidth and pixelHeight as
// little-endian 32-bit values. A 1D texture has a pixelHeight of 0.
std::optional<ImageHeader> readKtx2(ByteView bytes) {
    if (!startsWith(bytes, 0, "\xABKTX 20\xBB\r\n\x1A\n", 12) || !bytes.contains(20, 8)) {
        return std::nullopt;
    }
    return ImageHeader{ImageFormat::ktx2, littleEndian32(bytes.data + 20), littleEndian32(bytes.data + 24)};
}

} // namespace

std::optional<ImageHeader> readImageHeader(ByteView bytes) {
    for (const auto reader : {readPng, readJpeg, readWebp, readKtx2}) {
        if (auto header = reader(bytes)) {
            return header;
        }
    }
    return std::nullopt;
}

const char* mimeType(ImageFormat format) {
    switch (format) {
    case ImageFormat::png:
        return "image/png";
    case ImageFormat::jpeg:
        return "image/jpeg";
    case ImageFormat::webp:
        return "image/webp";
    case ImageFormat::ktx2:
        return "image/ktx2";
    }
    return "application/octet-stream";
}

} // namespace meshtinker::gltf
