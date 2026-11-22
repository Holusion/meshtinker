#include "commands/optimize/optimize.hpp"

#include "support/check.hpp"

#include <string>
#include <vector>

using namespace meshtinker;
using optimize::Model;
using optimize::Settings;

namespace {

// Records the steps the plan runs, with their options, and plays what each
// one would do to the model.
class Recorder : public optimize::Steps {
public:
    explicit Recorder(const Model& model) { model_ = model; }
    std::vector<std::string> steps;
    std::vector<std::vector<std::string>> options;
    // Faces simplify leaves, if it runs.
    std::uint64_t simplified_faces = 0;

protected:
    void execute(const std::string& command, const std::vector<std::string>& step_options) override {
        steps.push_back(command);
        options.push_back(step_options);
        if (command == "simplify" && simplified_faces > 0) {
            model_.faces = simplified_faces;
        }
    }
};

Model texturedScan() {
    Model model;
    model.primitives = 1;
    model.faces = 1000000;
    model.images = 2;
    model.largest_image = 8192;
    model.occupancy = 0.45;
    return model;
}

const optimize::Machine plenty{std::uint64_t{32} << 30};

std::vector<std::string> run(const Model& model, const Settings& settings,
                             const optimize::Machine& machine = plenty) {
    Recorder recorder(model);
    optimize::plan(recorder, settings, machine);
    return recorder.steps;
}

const std::vector<std::string>* optionsOf(const Recorder& recorder, const std::string& command) {
    for (std::size_t i = 0; i < recorder.steps.size(); ++i) {
        if (recorder.steps[i] == command) {
            return &recorder.options[i];
        }
    }
    return nullptr;
}

} // namespace

// A textured scan at the default settings goes through every step, in the
// step order of docs/chaining.md.
TEST_CASE(textured_scan_runs_every_step) {
    const std::vector<std::string> expected = {"normals", "weld", "dedup", "simplify", "uv", "ktx", "compress"};
    CHECK(run(texturedScan(), Settings{}) == expected);
}

// Quality picks simplify's error and the image size, after eCorpus's presets;
// Highest doesn't simplify.
TEST_CASE(quality_sets_error_and_image_size) {
    Settings settings;
    settings.quality = optimize::Quality::low;
    Recorder low(texturedScan());
    optimize::plan(low, settings, plenty);
    CHECK(*optionsOf(low, "simplify") == (std::vector<std::string>{"--error", "0.005"}));
    CHECK(*optionsOf(low, "ktx") == (std::vector<std::string>{"--quality", "Low", "--max-size", "1024"}));

    settings.quality = optimize::Quality::highest;
    Recorder highest(texturedScan());
    optimize::plan(highest, settings, plenty);
    CHECK(optionsOf(highest, "simplify") == nullptr);
    CHECK(*optionsOf(highest, "ktx") == (std::vector<std::string>{"--quality", "Highest", "--max-size", "8192"}));
}

// --textures avif swaps the texture step; the fast preset skips uv and
// encodes AVIF at a low effort whatever --textures says.
TEST_CASE(texture_format_and_fast_preset) {
    Settings settings;
    settings.textures = optimize::Textures::avif;
    CHECK(run(texturedScan(), settings) ==
          (std::vector<std::string>{"normals", "weld", "dedup", "simplify", "uv", "avif", "compress"}));

    settings = Settings{};
    settings.preset = optimize::Preset::fast;
    Recorder fast(texturedScan());
    optimize::plan(fast, settings, plenty);
    CHECK(fast.steps == (std::vector<std::string>{"normals", "weld", "dedup", "simplify", "avif", "compress"}));
    CHECK(*optionsOf(fast, "avif") ==
          (std::vector<std::string>{"--quality", "High", "--max-size", "4096", "--effort", "2"}));
}

// uv runs only when textures waste space, occupancy is known, and its xatlas
// data fits in half the memory; simplify's output counts, not import's.
TEST_CASE(uv_needs_waste_and_memory) {
    Model packed = texturedScan();
    packed.occupancy = 0.95;
    CHECK(run(packed, Settings{}) == (std::vector<std::string>{"normals", "weld", "dedup", "simplify", "ktx", "compress"}));

    Model unknown = texturedScan();
    unknown.occupancy.reset();
    CHECK(run(unknown, Settings{}).size() == 6);

    // 10 M faces need 650 MB: not with 1 GB, yes once simplify leaves 1 M.
    Model large = texturedScan();
    large.faces = 10000000;
    const optimize::Machine small{std::uint64_t{1} << 30};
    CHECK(run(large, Settings{}, small).size() == 6);
    Recorder simplified(large);
    simplified.simplified_faces = 1000000;
    optimize::plan(simplified, Settings{}, small);
    CHECK(simplified.steps.size() == 7 && simplified.steps[4] == "uv");
}

// Without textures there's no image step; without geometry, only dedup and
// the texture step.
TEST_CASE(steps_follow_content) {
    Model untextured = texturedScan();
    untextured.images = 0;
    untextured.occupancy.reset();
    CHECK(run(untextured, Settings{}) ==
          (std::vector<std::string>{"normals", "weld", "dedup", "simplify", "compress"}));

    Model images_only;
    images_only.images = 1;
    CHECK(run(images_only, Settings{}) == (std::vector<std::string>{"dedup", "ktx"}));
}
