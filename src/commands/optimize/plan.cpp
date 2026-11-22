// What optimize does to a model: which steps run, in which order, with which
// options. Everything else in optimize runs the steps; this file decides. Each
// condition reads what the steps so far reported (Steps::model), so a later
// step sees the model as the earlier ones left it. docs/optimize.md says why
// each rule is what it is.

#include "optimize.hpp"

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

// uv holds about 65 bytes per face of xatlas data (docs/uv.md#time-and-memory),
// and the machine must keep room for the rest.
bool uvFits(const Model& model, const Machine& machine) { return model.faces * 65 < machine.memory / 2; }

// A model whose textures are nearly all sampled has nothing for uv to repack.
constexpr double well_packed = 0.9;
bool texturesWasteSpace(const Model& model) { return model.occupancy && *model.occupancy < well_packed; }

// The fast preset encodes AVIF, at the effort sweep's fastest effort that is
// still mostly good (docs/experiments/webp-avif.md): KTX2 has no fast mode
// (docs/experiments/fast-textures.md).
constexpr int fast_avif_effort = 2;

std::string textureCommand(const Settings& settings) {
    return settings.preset == Preset::fast || settings.textures == Textures::avif ? "avif" : "ktx";
}

std::vector<std::string> textureOptions(const Settings& settings) {
    std::vector<std::string> options = {"--quality", qualityName(settings.quality), "--max-size",
                                        std::to_string(maxImageSize(settings))};
    if (settings.preset == Preset::fast) {
        options.insert(options.end(), {"--effort", std::to_string(fast_avif_effort)});
    }
    return options;
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

void plan(Steps& run, const Settings& settings, const Machine& machine) {
    const Model& model = run.model();
    const bool fast = settings.preset == Preset::fast;
    const double error = simplifyError(settings);

    if (model.hasGeometry()) run("normals");
    if (model.hasGeometry()) run("weld");
    run("dedup");
    if (model.hasGeometry() && error > 0) run("simplify", {"--error", number(error)});
    if (model.hasTextures() && !fast && texturesWasteSpace(model) && uvFits(model, machine)) run("uv");
    if (model.hasTextures()) run(textureCommand(settings), textureOptions(settings));
    if (model.hasGeometry()) run("compress");
}

} // namespace meshtinker::optimize
