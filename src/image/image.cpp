#include "meshtinker/image/image.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>

#ifdef MESHTINKER_DECODES_WEBP
#include <webp/decode.h>
#endif
#ifdef MESHTINKER_DECODES_AVIF
#include <avif/avif.h>
#endif

#include <stb/stb_image.h>
#include <transcoder/basisu_transcoder.h>
#include <stb/stb_image_resize2.h>
#include <stb/stb_image_write.h>

namespace meshtinker::image {

namespace {

bool startsWith(common::ByteView bytes, std::size_t offset, const char* text) {
    const std::size_t length = std::strlen(text);
    return bytes.size >= offset + length && std::memcmp(bytes.data + offset, text, length) == 0;
}

bool isWebp(common::ByteView bytes) { return startsWith(bytes, 0, "RIFF") && startsWith(bytes, 8, "WEBP"); }

// An ISOBMFF file: AVIF's `ftyp` box comes first.
bool isAvif(common::ByteView bytes) { return startsWith(bytes, 4, "ftyp"); }

#ifdef MESHTINKER_DECODES_WEBP
std::optional<Image> decodeWebp(common::ByteView bytes, std::string& error) {
    WebPBitstreamFeatures features;
    if (WebPGetFeatures(bytes.data, bytes.size, &features) != VP8_STATUS_OK || features.has_animation) {
        error = features.has_animation ? "animated WebP" : "not a WebP image libwebp reads";
        return std::nullopt;
    }
    Image image;
    image.width = static_cast<std::uint32_t>(features.width);
    image.height = static_cast<std::uint32_t>(features.height);
    image.source_channels = features.has_alpha ? 4 : 3;
    image.pixels.resize(image.texels() * 4);
    const int stride = features.width * 4;
    if (WebPDecodeRGBAInto(bytes.data, bytes.size, image.pixels.data(), image.pixels.size(), stride) == nullptr) {
        error = "libwebp couldn't decode it";
        return std::nullopt;
    }
    return image;
}
#endif

#ifdef MESHTINKER_DECODES_AVIF
std::optional<Image> decodeAvif(common::ByteView bytes, std::string& error) {
    std::unique_ptr<avifDecoder, void (*)(avifDecoder*)> decoder(avifDecoderCreate(), avifDecoderDestroy);
    if (!decoder) {
        throw std::bad_alloc();
    }
    // One thread: image commands decode several images at once (images.md).
    decoder->maxThreads = 1;
    avifResult result = avifDecoderSetIOMemory(decoder.get(), bytes.data, bytes.size);
    if (result == AVIF_RESULT_OK) {
        result = avifDecoderParse(decoder.get());
    }
    if (result == AVIF_RESULT_OK) {
        result = avifDecoderNextImage(decoder.get());
    }
    if (result == AVIF_RESULT_OUT_OF_MEMORY) {
        throw std::bad_alloc();
    }
    if (result != AVIF_RESULT_OK) {
        error = avifResultToString(result);
        return std::nullopt;
    }
    const avifImage* decoded = decoder->image;
    Image image;
    image.width = decoded->width;
    image.height = decoded->height;
    image.source_channels = decoded->alphaPlane != nullptr ? 4 : decoded->yuvFormat == AVIF_PIXEL_FORMAT_YUV400 ? 1 : 3;
    image.pixels.resize(image.texels() * 4);
    // Higher bit depths are reduced to 8, as stb_image does for PNG.
    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, decoded);
    rgb.format = AVIF_RGB_FORMAT_RGBA;
    rgb.depth = 8;
    rgb.pixels = image.pixels.data();
    rgb.rowBytes = image.width * 4;
    result = avifImageYUVToRGB(decoded, &rgb);
    if (result == AVIF_RESULT_OUT_OF_MEMORY) {
        throw std::bad_alloc();
    }
    if (result != AVIF_RESULT_OK) {
        error = avifResultToString(result);
        return std::nullopt;
    }
    return image;
}
#endif

bool isKtx2(common::ByteView bytes) { return startsWith(bytes, 0, "\xABKTX 20\xBB\r\n\x1A\n"); }

// KTX2 with ETC1S or UASTC, the codecs KHR_texture_basisu allows: the first
// level, layer and face, transcoded by basisu to RGBA8, as three.js's
// KTX2Loader transcodes for a GPU without a compressed format.
std::optional<Image> decodeKtx2(common::ByteView bytes, std::string& error) {
    static const bool initialized = (basist::basisu_transcoder_init(), true);
    static_cast<void>(initialized);
    if (bytes.size > std::numeric_limits<std::uint32_t>::max()) {
        error = "the KTX2 file is over 4 GiB";
        return std::nullopt;
    }
    basist::ktx2_transcoder transcoder;
    if (!transcoder.init(bytes.data, static_cast<std::uint32_t>(bytes.size))) {
        error = "not a KTX2 file basisu transcodes: only ETC1S and UASTC are";
        return std::nullopt;
    }
    if (!transcoder.is_etc1s() && !transcoder.is_uastc()) {
        error = "KTX2 other than ETC1S and UASTC";
        return std::nullopt;
    }
    if (!transcoder.start_transcoding()) {
        error = "the KTX2 file's ETC1S tables can't be read";
        return std::nullopt;
    }
    Image image;
    image.width = transcoder.get_width();
    image.height = transcoder.get_height();
    image.source_channels = transcoder.get_has_alpha() ? 4 : 3;
    if (image.texels() > std::numeric_limits<std::uint32_t>::max()) {
        error = "the KTX2 image has over 4 G texels";
        return std::nullopt;
    }
    image.pixels.resize(image.texels() * 4);
    if (!transcoder.transcode_image_level(0, 0, 0, image.pixels.data(), static_cast<std::uint32_t>(image.texels()),
                                          basist::transcoder_texture_format::cTFRGBA32)) {
        error = "basisu couldn't transcode the KTX2 file's first level";
        return std::nullopt;
    }
    return image;
}

} // namespace

bool decodesWebp() {
#ifdef MESHTINKER_DECODES_WEBP
    return true;
#else
    return false;
#endif
}

bool decodesAvif() {
#ifdef MESHTINKER_DECODES_AVIF
    return true;
#else
    return false;
#endif
}

std::optional<Image> decode(common::ByteView bytes, std::string& error) {
    if (isKtx2(bytes)) {
        return decodeKtx2(bytes, error);
    }
#ifdef MESHTINKER_DECODES_WEBP
    if (isWebp(bytes)) {
        return decodeWebp(bytes, error);
    }
#endif
#ifdef MESHTINKER_DECODES_AVIF
    if (isAvif(bytes)) {
        return decodeAvif(bytes, error);
    }
#endif
    if (isWebp(bytes) || isAvif(bytes)) {
        error = "this build doesn't decode WebP or AVIF";
        return std::nullopt;
    }
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
