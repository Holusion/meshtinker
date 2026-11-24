#pragma once

// Decoded images, for the commands that change pixels (`uv`, `ktx`, `avif`):
// decoding, channel analysis, resizing and writing (docs/images.md).

#include "meshtinker/common/bytes.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace meshtinker::image {

// RGBA, 8 bits per channel, rows top to bottom, no padding. Decoded pixels
// are on the heap: images are the exception to "map, don't load"
// (docs/images.md#memory-and-threads).
struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // Channels in the source file: 1 (gray), 2 (gray and alpha), 3 or 4.
    int source_channels = 4;
    std::vector<std::uint8_t> pixels;

    std::size_t texels() const { return static_cast<std::size_t>(width) * height; }
};

// Whether this build decodes WebP (libwebp) and AVIF (libavif): both are
// built from downloaded archives, and optional (MESHTINKER_WEBP and
// MESHTINKER_AVIF).
bool decodesWebp();
bool decodesAvif();

// Decodes PNG, JPEG, KTX2 (ETC1S and UASTC, the first level), and WebP and
// AVIF where the build decodes them, from `bytes`, the mapped file, to RGBA8.
// Higher bit depths are reduced to 8 bits; an animated WebP isn't decoded. Empty if the bytes can't be decoded, with the decoder's
// reason in `error`. Throws std::bad_alloc if the pixels don't fit in memory.
std::optional<Image> decode(common::ByteView bytes, std::string& error);

struct Channels {
    // Some alpha value isn't 255.
    bool alpha = false;
    // Some texel has R, G and B not all equal.
    bool color = false;
};

// Which channels hold information (docs/images.md#channel-analysis).
Channels analyze(const Image& image);

// How the pixels are interpreted, which decides how they are filtered.
enum class Content : std::uint8_t {
    // sRGB color, filtered in linear light, alpha premultiplied.
    srgb,
    // Linear data, each channel filtered on its own.
    linear,
    // A tangent-space normal map in RGB: linear, renormalized after filtering.
    normal,
};

struct Size {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool operator==(const Size& other) const { return width == other.width && height == other.height; }
    bool operator!=(const Size& other) const { return !(*this == other); }
};

// The size an image of `size` is written at: its longest side capped at
// `max_size` (0 for no cap) keeping the aspect ratio, never enlarged, then
// each side rounded up to a multiple of `multiple` (docs/images.md#resizing).
Size fittedSize(Size size, std::uint32_t max_size, std::uint32_t multiple);

// Resamples `image` to `size` with stb_image_resize2's default filters.
// `alpha` says whether the alpha channel is used: when it is, color is
// premultiplied by it while filtering.
Image resize(const Image& image, Size size, Content content, bool alpha);

// Encodes `image` as PNG, with `channels` channels (3 drops alpha), at
// compression level 1: writing time matters more than size here
// (docs/images.md#writing).
std::vector<std::uint8_t> encodePng(const Image& image, int channels);

// The number of CPUs this process may use: its affinity mask, capped by the
// cgroup's cpu.max quota when one is set. At least 1.
unsigned threadCount();

} // namespace meshtinker::image
