// What optimize does to a model: which steps run, in which order, with which
// options. Everything else in optimize runs the steps; this file decides. Each
// condition reads what the steps so far reported (Steps::model), so a later
// step sees the model as the earlier ones left it. docs/optimize.md says why
// each rule is what it is, and docs/experiments/optimize-survey.md measured
// the figures below on the local corpus.

#include "optimize.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace meshtinker::optimize {
namespace {

// Per quality, after eCorpus's getPreset (optimizeGlb.ts): how far simplify
// may move the surface, as a fraction of the scene's size (0: not at all),
// and the longest side of an image.
double simplifyError(const Settings& settings) {
    switch (settings.quality) {
    case Quality::highest:
        return 0;
    case Quality::high:
        return 0.0001;
    case Quality::medium:
        return 0.001;
    case Quality::low:
        return 0.005;
    case Quality::thumb:
        return 0.01;
    }
    return 0.001;
}

std::uint32_t maxImageSize(const Settings& settings) {
    switch (settings.quality) {
    case Quality::highest:
        return 8192;
    case Quality::high:
        return 4096;
    case Quality::medium:
        return 2048;
    case Quality::low:
        return 1024;
    case Quality::thumb:
        return 512;
    }
    return 2048;
}

// GPU memory the textures and geometry should fit in, best effort: textures
// are made smaller down to budget_floor at most, and the geometry is what
// simplify's error leaves of it.
constexpr double gpu_budget = 256.0 * 1024 * 1024;
constexpr std::uint32_t budget_floor = 1024;
constexpr std::uint32_t side_step = 256;

// Images this small are left as they are: they cost little GPU memory, and
// KTX2 gains little on them. Unless there are so many that the model doesn't
// fit the budget without encoding them (minSize).
constexpr std::uint32_t small_image = 512;

// The fast preset sizes its textures for AVIF to take about this long, from
// the survey's cost on 4 cores: 0.13 s per output megatexel at effort 2.
// simplify isn't bounded: a model left unsimplified can be too heavy to look
// at, which is worse than a long wait (docs/optimize.md#presets).
constexpr double fast_step_seconds = 6;
constexpr double avif_seconds_per_texel = 0.13e-6;
constexpr std::uint32_t fast_floor = 512;

// uv holds about 65 bytes per face of xatlas data (docs/uv.md#time-and-memory),
// and the machine must keep room for the rest.
bool uvFits(const Model& model, const Machine& machine) { return model.faces * 65 < machine.memory / 2; }

// uv repacked no model above this occupancy in the survey, apart from cards
// of a few faces.
constexpr double well_packed = 0.6;
bool texturesWasteSpace(const Model& model) { return model.occupancy && *model.occupancy < well_packed; }

// slow writes KTX2, which stays compressed on the GPU. fast writes AVIF, at the
// effort sweep's fastest effort that is still mostly good
// (docs/experiments/webp-avif.md): KTX2 has no fast mode
// (docs/experiments/fast-textures.md).
constexpr int fast_avif_effort = 2;

std::string textureCommand(const Settings& settings) {
    return settings.preset == Preset::fast ? "avif" : "ktx";
}

// An image the texture step encodes: over `min_size`, not KTX2 already.
bool encoded(const Image& image, std::uint32_t min_size) { return !image.ktx2 && image.longestSide() > min_size; }

// An image's texels once its longest side is at most `side`, as
// image::fittedSize scales it.
double texelsAt(const Image& image, std::uint32_t side) {
    const double scale = image.longestSide() > side ? double(side) / image.longestSide() : 1.0;
    return std::floor(image.width * scale) * std::floor(image.height * scale);
}

// A KTX2 image that ktx encodes again: only when its side shrinks it, which
// accepts a second lossy round for the memory (docs/images.md#formats). avif
// leaves KTX2 as it is.
bool reencoded(const Image& image, const Settings& settings, std::uint32_t side) {
    return image.ktx2 && textureCommand(settings) == "ktx" && image.longestSide() > side;
}

// The textures' GPU memory once the texture step has encoded them at `side`.
double textureGpuBytesAt(const Model& model, const Settings& settings, std::uint32_t side, std::uint32_t min_size) {
    double bytes = 0;
    for (const Image& image : model.images) {
        if (reencoded(image, settings, side)) {
            bytes += texelsAt(image, side) * (image.color ? 0.5 : 1.0);
        } else if (!encoded(image, min_size)) {
            bytes += double(image.width) * image.height * image.gpu_bytes_per_texel;
        } else if (textureCommand(settings) == "ktx") {
            bytes += texelsAt(image, side) * (image.color ? 0.5 : 1.0);
        } else {
            bytes += texelsAt(image, side) * 4;
        }
    }
    return bytes * 4 / 3;
}

double encodeSecondsAt(const Model& model, std::uint32_t side, std::uint32_t min_size) {
    double texels = 0;
    for (const Image& image : model.images) {
        texels += encoded(image, min_size) ? texelsAt(image, side) : 0;
    }
    return texels * avif_seconds_per_texel;
}

// Small images are encoded too when the model doesn't fit the budget at the
// floor without them, and encoding them saves memory: with ktx, not with
// avif, which uploads as RGBA8 like the sources (the campus scan's 634
// images, most of 512 and less).
std::uint32_t minSize(const Model& model, const Settings& settings) {
    const double room = gpu_budget - geometryGpuBytes(model);
    const bool small_ones = std::any_of(model.images.begin(), model.images.end(), [](const Image& image) {
        return encoded(image, 0) && !encoded(image, small_image);
    });
    const bool fits = textureGpuBytesAt(model, settings, budget_floor, small_image) <= room;
    return small_ones && !fits && textureCommand(settings) == "ktx" ? 0 : small_image;
}

// The largest side, in steps of 256 under the quality's, whose textures fit
// the GPU budget with the geometry, and in fast, whose encoding fits its time.
std::uint32_t textureSide(const Model& model, const Settings& settings, std::uint32_t min_size) {
    std::uint32_t side = maxImageSize(settings);
    const double room = gpu_budget - geometryGpuBytes(model);
    while (side > budget_floor && textureGpuBytesAt(model, settings, side, min_size) > room) {
        side = std::max(budget_floor, side - side_step);
    }
    while (settings.preset == Preset::fast && side > fast_floor &&
           encodeSecondsAt(model, side, min_size) > fast_step_seconds) {
        side = std::max(fast_floor, side - side_step);
    }
    return side;
}

// KTX2 images that the texture step's side shrinks, and that ktx encodes again.
bool hasKtx2ToShrink(const Model& model, const Settings& settings) {
    const std::uint32_t side = textureSide(model, settings, minSize(model, settings));
    return std::any_of(model.images.begin(), model.images.end(),
                       [&](const Image& image) { return reencoded(image, settings, side); });
}

bool hasImagesToEncode(const Model& model, const Settings& settings) {
    const std::uint32_t min_size = minSize(model, settings);
    return std::any_of(model.images.begin(), model.images.end(),
                       [&](const Image& image) { return encoded(image, min_size); }) ||
           hasKtx2ToShrink(model, settings);
}

// uv bakes KTX2 images only when its --max-size shrinks them, as ktx encodes
// them again: given the texture step's side when ktx will shrink some, so that
// it repacks them first (docs/optimize.md#plan).
std::vector<std::string> uvOptions(const Model& model, const Settings& settings) {
    if (!hasKtx2ToShrink(model, settings)) {
        return {};
    }
    return {"--max-size", std::to_string(textureSide(model, settings, minSize(model, settings)))};
}

bool overBudget(const Model& model) { return geometryGpuBytes(model) + textureGpuBytes(model) > gpu_budget; }

std::vector<std::string> textureOptions(const Model& model, const Settings& settings) {
    const std::uint32_t min_size = minSize(model, settings);
    std::vector<std::string> options = {"--quality",  qualityName(settings.quality),
                                        "--max-size", std::to_string(textureSide(model, settings, min_size)),
                                        "--min-size", std::to_string(min_size)};
    if (settings.preset == Preset::fast) {
        options.insert(options.end(), {"--effort", std::to_string(fast_avif_effort)});
    }
    return options;
}

// Compressed geometry stays as it is while it fits: simplify would write it
// uncompressed, and compress's meshopt can be larger than its Draco.
bool keepsCompression(const Model& model) {
    return model.geometryIsCompressed() && geometryGpuBytes(model) <= gpu_budget;
}

std::string megabytes(double bytes) { return std::to_string(std::lround(bytes / (1024 * 1024))) + " MB"; }

std::string overBudgetMessage(const Model& model) {
    return "the model needs about " + megabytes(geometryGpuBytes(model) + textureGpuBytes(model)) +
           " of GPU memory, over the " + megabytes(gpu_budget) + " budget: textures go no smaller than " +
           std::to_string(budget_floor) + ", and geometry only as small as simplify's error";
}

std::string number(double value) {
    std::string text = std::to_string(value);
    text.erase(text.find_last_not_of('0') + 1);
    if (text.back() == '.') {
        text.pop_back();
    }
    return text;
}

} // namespace

double geometryGpuBytes(const Model& model) { return double(model.vertices) * 16 + double(model.faces) * 12; }

double textureGpuBytes(const Model& model) {
    double bytes = 0;
    for (const Image& image : model.images) {
        bytes += double(image.width) * image.height * image.gpu_bytes_per_texel;
    }
    return bytes * 4 / 3;
}

void plan(Steps& run, const Settings& settings, const Machine& machine) {
    const Model& model = run.model();
    const bool fast = settings.preset == Preset::fast;
    const double error = simplifyError(settings);
    const auto simplifies = [&] { return model.hasGeometry() && error > 0 && !keepsCompression(model); };

    if (model.hasGeometry()) run("normals");
    if (model.hasGeometry()) run("weld");
    run("dedup");
    if (simplifies()) run("simplify", {"--error", number(error)});
    if (model.hasTextures() && !fast && texturesWasteSpace(model) && uvFits(model, machine)) {
        run("uv", uvOptions(model, settings));
    }
    if (hasImagesToEncode(model, settings)) run(textureCommand(settings), textureOptions(model, settings));
    if (overBudget(model)) run.warn("gpu-budget-exceeded", overBudgetMessage(model));
    if (model.hasGeometry()) run("compress");
}

} // namespace meshtinker::optimize
