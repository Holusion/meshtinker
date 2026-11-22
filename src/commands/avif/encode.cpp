#include "encode.hpp"

#include <avif/avif.h>

#include <limits>
#include <memory>

namespace meshtinker::avif {

namespace {

struct ImageDeleter {
    void operator()(avifImage* image) const { avifImageDestroy(image); }
};
struct EncoderDeleter {
    void operator()(avifEncoder* encoder) const { avifEncoderDestroy(encoder); }
};

avifPixelFormat pixelFormat(Subsampling subsampling) {
    switch (subsampling) {
    case Subsampling::yuv420:
        return AVIF_PIXEL_FORMAT_YUV420;
    case Subsampling::yuv444:
        return AVIF_PIXEL_FORMAT_YUV444;
    case Subsampling::yuv400:
        return AVIF_PIXEL_FORMAT_YUV400;
    }
    return AVIF_PIXEL_FORMAT_YUV444;
}

} // namespace

Encoder::Encoder(unsigned threads, int quality, int effort) : threads_(threads), quality_(quality), effort_(effort) {}

std::optional<std::vector<std::uint8_t>> Encoder::encode(image::Image image, const texture::Request& request,
                                                         std::string& error) {
    const Subsampling format = subsampling(request, image.source_channels);
    const std::unique_ptr<avifImage, ImageDeleter> avif(
        avifImageCreate(image.width, image.height, 8, pixelFormat(format)));
    if (!avif) {
        error = "can't allocate the image";
        return std::nullopt;
    }
    // The sRGB description for every image (docs/avif.md#color-description):
    // browsers then convert nothing, as for an untagged PNG or JPEG, and
    // three.js reads each slot as sRGB or linear by itself.
    avif->colorPrimaries = AVIF_COLOR_PRIMARIES_BT709;
    avif->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SRGB;
    avif->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT601;
    avif->yuvRange = AVIF_RANGE_FULL;

    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, avif.get());
    rgb.format = AVIF_RGB_FORMAT_RGBA;
    rgb.depth = 8;
    rgb.pixels = image.pixels.data();
    rgb.rowBytes = image.width * 4;
    // Only an alpha that a slot samples is kept (docs/avif.md#per-slot-settings).
    rgb.ignoreAlpha = request.alpha ? AVIF_FALSE : AVIF_TRUE;
    avifResult result = avifImageRGBToYUV(avif.get(), &rgb);
    if (result != AVIF_RESULT_OK) {
        error = avifResultToString(result);
        return std::nullopt;
    }
    image.pixels = {};
    image.pixels.shrink_to_fit();

    const std::unique_ptr<avifEncoder, EncoderDeleter> encoder(avifEncoderCreate());
    if (!encoder) {
        error = "can't allocate the encoder";
        return std::nullopt;
    }
    encoder->codecChoice = AVIF_CODEC_CHOICE_AOM;
    encoder->maxThreads = threads_ > static_cast<unsigned>(std::numeric_limits<int>::max())
                              ? std::numeric_limits<int>::max()
                              : static_cast<int>(threads_);
    // sharp's and vips's effort, 0 (fastest) to 9, is libavif's speed 9 to 0.
    encoder->speed = 9 - effort_;
    encoder->quality = quality_;
    encoder->qualityAlpha = quality_;
    // Tiles let libaom spread a large image over every thread.
    encoder->autoTiling = AVIF_TRUE;

    avifRWData output = AVIF_DATA_EMPTY;
    result = avifEncoderWrite(encoder.get(), avif.get(), &output);
    if (result != AVIF_RESULT_OK) {
        error = avifResultToString(result);
        if (encoder->diag.error[0] != '\0') {
            error += std::string(": ") + encoder->diag.error;
        }
        avifRWDataFree(&output);
        return std::nullopt;
    }
    std::vector<std::uint8_t> file(output.data, output.data + output.size);
    avifRWDataFree(&output);
    return file;
}

} // namespace meshtinker::avif
