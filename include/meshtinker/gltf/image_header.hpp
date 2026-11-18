#pragma once

#include "meshtinker/common/bytes.hpp"

#include <cstdint>
#include <optional>

namespace meshtinker::gltf {

enum class ImageFormat : std::uint8_t { png, jpeg, webp, ktx2 };

struct ImageHeader {
    ImageFormat format;
    std::uint32_t width;
    std::uint32_t height;
};

// Reads an image's format and dimensions from its first bytes, without
// decoding pixels. Recognizes PNG, JPEG, WebP (lossy, lossless and extended)
// and KTX2. Empty if the bytes are none of these, or too short to say.
std::optional<ImageHeader> readImageHeader(common::ByteView bytes);

const char* mimeType(ImageFormat format);

} // namespace meshtinker::gltf
