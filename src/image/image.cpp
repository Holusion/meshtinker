#include "meshtinker/image/image.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

#include <stb/stb_image.h>
#include <stb/stb_image_resize2.h>
#include <stb/stb_image_write.h>

namespace meshtinker::image {

std::optional<Image> decode(common::ByteView bytes, std::string& error) {
    if (bytes.size > static_cast<std::size_t>(INT_MAX)) {
        error = "the file is over 2 GiB";
        return std::nullopt;
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    // Always 8 bits: stb_image reduces 16-bit PNG to 8 (docs/images.md#formats).
    stbi_uc* pixels = stbi_load_from_memory(bytes.data, static_cast<int>(bytes.size), &width, &height, &channels, 4);
    if (pixels == nullptr) {
        const char* reason = stbi_failure_reason();
        error = reason != nullptr ? reason : "unknown error";
        if (error == "outofmem") {
            throw std::bad_alloc();
        }
        return std::nullopt;
    }
    Image image;
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    image.source_channels = channels;
    try {
        image.pixels.assign(pixels, pixels + image.texels() * 4);
    } catch (...) {
        stbi_image_free(pixels);
        throw;
    }
    stbi_image_free(pixels);
    return image;
}

Channels analyze(const Image& image) {
    Channels channels;
    const std::uint8_t* p = image.pixels.data();
    const std::uint8_t* end = p + image.pixels.size();
    for (; p != end && !(channels.alpha && channels.color); p += 4) {
        channels.alpha = channels.alpha || p[3] != 255;
        channels.color = channels.color || p[0] != p[1] || p[1] != p[2];
    }
    return channels;
}

namespace {

std::uint32_t roundUp(std::uint32_t value, std::uint32_t multiple) {
    if (multiple <= 1) {
        return value;
    }
    return (value + multiple - 1) / multiple * multiple;
}

std::uint32_t scaled(std::uint32_t side, double scale) {
    return std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(side * scale)));
}

// Renormalizes the RGB of each texel as a unit vector in [-1, 1]³.
void renormalize(Image& image) {
    std::uint8_t* p = image.pixels.data();
    std::uint8_t* end = p + image.pixels.size();
    for (; p != end; p += 4) {
        double v[3];
        for (int c = 0; c < 3; ++c) {
            v[c] = p[c] / 127.5 - 1.0;
        }
        const double length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (length < 1e-6) {
            continue;
        }
        for (int c = 0; c < 3; ++c) {
            const double encoded = (v[c] / length + 1.0) * 127.5;
            p[c] = static_cast<std::uint8_t>(std::clamp(std::lround(encoded), 0L, 255L));
        }
    }
}

void appendBytes(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<std::uint8_t>*>(context);
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

} // namespace

Size fittedSize(Size size, std::uint32_t max_size, std::uint32_t multiple) {
    const std::uint32_t longest = std::max(size.width, size.height);
    Size fitted = size;
    if (max_size != 0 && longest > max_size) {
        const double scale = static_cast<double>(max_size) / longest;
        fitted = {std::min(max_size, scaled(size.width, scale)), std::min(max_size, scaled(size.height, scale))};
    }
    return {roundUp(fitted.width, multiple), roundUp(fitted.height, multiple)};
}

Image resize(const Image& image, Size size, Content content, bool alpha) {
    if (size.width > static_cast<std::uint32_t>(INT_MAX / 4) || size.height > static_cast<std::uint32_t>(INT_MAX) ||
        image.width > static_cast<std::uint32_t>(INT_MAX / 4) || image.height > static_cast<std::uint32_t>(INT_MAX)) {
        throw std::length_error("image too large to resize");
    }
    Image out;
    out.width = size.width;
    out.height = size.height;
    out.source_channels = image.source_channels;
    out.pixels.resize(out.texels() * 4);
    // STBIR_RGBA premultiplies alpha while filtering; STBIR_4CHANNEL filters
    // each channel on its own. The sRGB type filters color in linear light
    // and alpha linearly.
    const stbir_pixel_layout layout = content == Content::srgb && alpha ? STBIR_RGBA : STBIR_4CHANNEL;
    const stbir_datatype type = content == Content::srgb ? STBIR_TYPE_UINT8_SRGB : STBIR_TYPE_UINT8;
    const void* result =
        stbir_resize(image.pixels.data(), static_cast<int>(image.width), static_cast<int>(image.height),
                     static_cast<int>(image.width * 4), out.pixels.data(), static_cast<int>(out.width),
                     static_cast<int>(out.height), static_cast<int>(out.width * 4), layout, type, STBIR_EDGE_CLAMP,
                     STBIR_FILTER_DEFAULT);
    if (result == nullptr) {
        throw std::bad_alloc();
    }
    if (content == Content::normal) {
        renormalize(out);
    }
    return out;
}

std::vector<std::uint8_t> encodePng(const Image& image, int channels) {
    if (image.width > static_cast<std::uint32_t>(INT_MAX / 4) || image.height > static_cast<std::uint32_t>(INT_MAX)) {
        throw std::length_error("image too large to write as PNG");
    }
    const Image* source = &image;
    Image rgb;
    if (channels == 3) {
        rgb.width = image.width;
        rgb.height = image.height;
        rgb.pixels.resize(image.texels() * 3);
        for (std::size_t i = 0; i < image.texels(); ++i) {
            std::memcpy(&rgb.pixels[i * 3], &image.pixels[i * 4], 3);
        }
        source = &rgb;
    }
    // A global of stb_image_write's: set once, so that threads don't race on it.
    static const bool level_set = (stbi_write_png_compression_level = 1, true);
    static_cast<void>(level_set);
    std::vector<std::uint8_t> out;
    const int ok = stbi_write_png_to_func(appendBytes, &out, static_cast<int>(source->width),
                                          static_cast<int>(source->height), channels, source->pixels.data(),
                                          static_cast<int>(source->width) * channels);
    if (ok == 0) {
        throw std::bad_alloc();
    }
    return out;
}

} // namespace meshtinker::image
