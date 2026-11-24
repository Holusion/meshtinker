#include "meshtinker/gltf/image_header.hpp"

#include <cstring>
#include <vector>

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

// The codec of a KTX2 file (KTX 2.0 specification, sections 3 and 4): BasisLZ
// supercompression (scheme 1) is only defined for ETC1S. Otherwise a vkFormat
// of 0 says the data format descriptor names the codec: the colorModel byte of
// its basic block, 163 for ETC1S, 166 for UASTC. dfdByteOffset is at byte 48;
// the block's colorModel is 12 bytes into the descriptor, after its total size
// and the block's first two words.
Ktx2Codec ktx2Codec(ByteView bytes) {
    constexpr std::uint32_t basis_lz = 1;
    constexpr std::uint8_t model_etc1s = 163;
    constexpr std::uint8_t model_uastc = 166;
    if (!bytes.contains(12, 4) || !bytes.contains(44, 8)) {
        return Ktx2Codec::other;
    }
    if (littleEndian32(bytes.data + 44) == basis_lz) {
        return Ktx2Codec::etc1s;
    }
    if (littleEndian32(bytes.data + 12) != 0) {
        return Ktx2Codec::other;
    }
    const std::size_t dfd = littleEndian32(bytes.data + 48);
    if (!bytes.contains(dfd + 12, 1)) {
        return Ktx2Codec::other;
    }
    switch (bytes.data[dfd + 12]) {
    case model_etc1s:
        return Ktx2Codec::etc1s;
    case model_uastc:
        return Ktx2Codec::uastc;
    default:
        return Ktx2Codec::other;
    }
}

// KTX2: the identifier, then vkFormat, typeSize, pixelWidth and pixelHeight as
// little-endian 32-bit values. A 1D texture has a pixelHeight of 0.
std::optional<ImageHeader> readKtx2(ByteView bytes) {
    if (!startsWith(bytes, 0, "\xABKTX 20\xBB\r\n\x1A\n", 12) || !bytes.contains(20, 8)) {
        return std::nullopt;
    }
    return ImageHeader{ImageFormat::ktx2, littleEndian32(bytes.data + 20), littleEndian32(bytes.data + 24),
                       ktx2Codec(bytes)};
}

// An ISOBMFF box: its type, and where its payload and the box end.
struct Box {
    char type[4];
    std::size_t payload;
    std::size_t end;
};

// The box at `offset`, which must end by `limit`.
std::optional<Box> boxAt(ByteView bytes, std::size_t offset, std::size_t limit) {
    if (!bytes.contains(offset, 8) || offset + 8 > limit) {
        return std::nullopt;
    }
    std::uint64_t size = bigEndian32(bytes.data + offset);
    std::size_t header = 8;
    if (size == 1) {
        if (!bytes.contains(offset, 16)) {
            return std::nullopt;
        }
        size = (std::uint64_t{bigEndian32(bytes.data + offset + 8)} << 32) | bigEndian32(bytes.data + offset + 12);
        header = 16;
    } else if (size == 0) {
        size = limit - offset;
    }
    if (size < header || size > limit - offset) {
        return std::nullopt;
    }
    Box box{};
    std::memcpy(box.type, bytes.data + offset + 4, 4);
    box.payload = offset + header;
    box.end = offset + static_cast<std::size_t>(size);
    return box;
}

// The first child of type `type` among the boxes from `offset` to `limit`.
std::optional<Box> findBox(ByteView bytes, std::size_t offset, std::size_t limit, const char* type) {
    while (offset < limit) {
        const std::optional<Box> box = boxAt(bytes, offset, limit);
        if (!box) {
            return std::nullopt;
        }
        if (std::memcmp(box->type, type, 4) == 0) {
            return box;
        }
        offset = box->end;
    }
    return std::nullopt;
}

// AVIF: an ISOBMFF file whose `ftyp` box lists the `avif` or `avis` brand.
// Its size is the `ispe` property (image spatial extents) of the primary item:
// the `pitm` item, through the `ipma` associations, since a grid or alpha item
// has its own.
std::optional<ImageHeader> readAvif(ByteView bytes) {
    const std::optional<Box> ftyp = boxAt(bytes, 0, bytes.size);
    if (!ftyp || std::memcmp(ftyp->type, "ftyp", 4) != 0 || ftyp->end - ftyp->payload < 8) {
        return std::nullopt;
    }
    bool brand = false;
    for (std::size_t at = ftyp->payload; at + 4 <= ftyp->end; at += at == ftyp->payload ? std::size_t{8} : std::size_t{4}) {
        brand = brand || startsWith(bytes, at, "avif", 4) || startsWith(bytes, at, "avis", 4);
    }
    if (!brand) {
        return std::nullopt;
    }
    // `meta`, `iprp` and their children are full boxes or plain containers:
    // `meta` has 4 bytes of version and flags before its children.
    const std::optional<Box> meta = findBox(bytes, ftyp->end, bytes.size, "meta");
    if (!meta || meta->end - meta->payload < 4) {
        return std::nullopt;
    }
    const std::size_t children = meta->payload + 4;
    std::optional<std::uint32_t> primary;
    if (const std::optional<Box> pitm = findBox(bytes, children, meta->end, "pitm"); pitm && bytes.contains(pitm->payload, 6)) {
        primary = bytes.data[pitm->payload] == 0 ? bigEndian16(bytes.data + pitm->payload + 4)
                                                 : (bytes.contains(pitm->payload, 8) ? bigEndian32(bytes.data + pitm->payload + 4) : 0);
    }
    const std::optional<Box> iprp = findBox(bytes, children, meta->end, "iprp");
    const std::optional<Box> ipco = iprp ? findBox(bytes, iprp->payload, iprp->end, "ipco") : std::nullopt;
    if (!ipco) {
        return std::nullopt;
    }
    // Property indices are 1-based, in `ipco` order.
    struct Extent {
        std::uint32_t index;
        std::uint32_t width;
        std::uint32_t height;
    };
    std::vector<Extent> extents;
    std::uint32_t index = 0;
    for (std::size_t at = ipco->payload; at < ipco->end;) {
        const std::optional<Box> property = boxAt(bytes, at, ipco->end);
        if (!property) {
            return std::nullopt;
        }
        ++index;
        if (std::memcmp(property->type, "ispe", 4) == 0 && property->end - property->payload >= 12) {
            extents.push_back({index, bigEndian32(bytes.data + property->payload + 4), bigEndian32(bytes.data + property->payload + 8)});
        }
        at = property->end;
    }
    if (extents.empty()) {
        return std::nullopt;
    }
    const auto header = [](const Extent& extent) { return ImageHeader{ImageFormat::avif, extent.width, extent.height}; };
    const std::optional<Box> ipma = findBox(bytes, iprp->payload, iprp->end, "ipma");
    if (!primary || !ipma || !bytes.contains(ipma->payload, 8)) {
        return header(extents.front());
    }
    const std::uint8_t version = bytes.data[ipma->payload];
    const bool wide_index = (bytes.data[ipma->payload + 3] & 1) != 0;
    const std::uint32_t entries = bigEndian32(bytes.data + ipma->payload + 4);
    std::size_t at = ipma->payload + 8;
    for (std::uint32_t e = 0; e < entries; ++e) {
        const std::size_t id_size = version < 1 ? 2 : 4;
        if (at + id_size + 1 > ipma->end) {
            break;
        }
        const std::uint32_t item = version < 1 ? bigEndian16(bytes.data + at) : bigEndian32(bytes.data + at);
        const std::uint32_t associations = bytes.data[at + id_size];
        at += id_size + 1;
        for (std::uint32_t a = 0; a < associations; ++a) {
            const std::size_t size = wide_index ? 2 : 1;
            if (at + size > ipma->end) {
                return header(extents.front());
            }
            const std::uint32_t property = wide_index ? bigEndian16(bytes.data + at) & 0x7FFFU : bytes.data[at] & 0x7FU;
            at += size;
            if (item != *primary) {
                continue;
            }
            for (const Extent& extent : extents) {
                if (extent.index == property) {
                    return header(extent);
                }
            }
        }
    }
    return header(extents.front());
}

} // namespace

std::optional<ImageHeader> readImageHeader(ByteView bytes) {
    for (const auto reader : {readPng, readJpeg, readWebp, readKtx2, readAvif}) {
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
    case ImageFormat::avif:
        return "image/avif";
    }
    return "application/octet-stream";
}

const char* codecName(Ktx2Codec codec) {
    switch (codec) {
    case Ktx2Codec::etc1s:
        return "ETC1S";
    case Ktx2Codec::uastc:
        return "UASTC";
    case Ktx2Codec::other:
        break;
    }
    return "other";
}

} // namespace meshtinker::gltf
