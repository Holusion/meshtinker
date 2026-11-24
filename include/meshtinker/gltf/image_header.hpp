#pragma once

#include "meshtinker/common/bytes.hpp"

#include <cstdint>
#include <optional>

namespace meshtinker::gltf {

enum class ImageFormat : std::uint8_t { png, jpeg, webp, ktx2, avif };

// What a KTX2 image holds. KHR_texture_basisu allows ETC1S and UASTC; `other`
// is a KTX2 that stores a GPU format directly (a vkFormat other than 0), or
// one whose data format descriptor can't be read.
enum class Ktx2Codec : std::uint8_t { etc1s, uastc, other };

struct ImageHeader {
    ImageFormat format;
    std::uint32_t width;
    std::uint32_t height;
    // KTX2 only.
    std::optional<Ktx2Codec> ktx2_codec = std::nullopt;
};

// Reads an image's format and dimensions from its first bytes, without
// decoding pixels. Recognizes PNG, JPEG, WebP (lossy, lossless and extended),
// KTX2 and AVIF. Empty if the bytes are none of these, or too short to say.
std::optional<ImageHeader> readImageHeader(common::ByteView bytes);

const char* mimeType(ImageFormat format);

// "ETC1S", "UASTC" or "other", as the inspect report writes them, and as ktx
// names the codecs it writes.
const char* codecName(Ktx2Codec codec);

} // namespace meshtinker::gltf
