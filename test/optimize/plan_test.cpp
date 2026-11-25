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
    std::vector<std::string> warnings;
    // Faces simplify leaves, if it runs.
    std::uint64_t simplified_faces = 0;

protected:
    void execute(const std::string& command, const std::vector<std::string>& step_options) override {
        steps.push_back(command);
        options.push_back(step_options);
        if (command == "simplify" && simplified_faces > 0) {
            model_.faces = simplified_faces;
            model_.vertices = simplified_faces / 2;
        }
        if (command == "ktx") {
            // Encodes every image over --min-size at --max-size, ETC1S for
            // color, and KTX2 ones only if --max-size shrinks them.
            const std::uint32_t side = static_cast<std::uint32_t>(std::stoul(step_options[3]));
            const std::uint32_t min_size = static_cast<std::uint32_t>(std::stoul(step_options[5]));
            for (optimize::Image& image : model_.images) {
                if (image.ktx2 ? image.longestSide() > side : image.longestSide() > min_size) {
                    if (image.longestSide() > side) {
                        image.width = static_cast<std::uint32_t>(std::uint64_t{image.width} * side / image.longestSide());
                        image.height = static_cast<std::uint32_t>(std::uint64_t{image.height} * side / image.longestSide());
                    }
                    image.ktx2 = true;
                    image.gpu_bytes_per_texel = image.color ? 0.5 : 1;
                }
            }
        }
    }
    void warned(const std::string& code, const std::string&) override { warnings.push_back(code); }
};

optimize::Image image(std::uint32_t side, bool color) {
    optimize::Image image;
    image.file = std::to_string(side) + (color ? "-color.png" : "-normal.png");
    image.width = side;
    image.height = side;
    image.color = color;
    return image;
}

// A million faces, an 8k color map and an 8k normal map.
Model texturedScan() {
    Model model;
    model.primitives = 1;
    model.faces = 1000000;
    model.vertices = 500000;
    model.images = {image(8192, true), image(8192, false)};
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

std::vector<std::string> textureOptions(const char* quality, const char* max_size, const char* min_size = "512") {
    return {"--quality", quality, "--max-size", max_size, "--min-size", min_size};
}

} // namespace

// A textured scan at the default settings goes through every step, in the
// step order of docs/chaining.md.
TEST_CASE(textured_scan_runs_every_step) {
    const std::vector<std::string> expected = {"normals", "weld", "dedup", "simplify", "uv", "ktx", "compress"};
    Recorder recorder(texturedScan());
    optimize::plan(recorder, Settings{}, plenty);
    CHECK(recorder.steps == expected);
    CHECK(recorder.warnings.empty());
}

// Quality picks simplify's error and the image size, after eCorpus's presets;
// Highest doesn't simplify.
TEST_CASE(quality_sets_error_and_image_size) {
    Settings settings;
    settings.quality = optimize::Quality::low;
    Recorder low(texturedScan());
    optimize::plan(low, settings, plenty);
    CHECK(*optionsOf(low, "simplify") == (std::vector<std::string>{"--error", "0.005"}));
    CHECK(*optionsOf(low, "ktx") == textureOptions("Low", "1024"));

    settings.quality = optimize::Quality::highest;
    Recorder highest(texturedScan());
    optimize::plan(highest, settings, plenty);
    CHECK(optionsOf(highest, "simplify") == nullptr);
    CHECK(*optionsOf(highest, "ktx") == textureOptions("Highest", "8192"));
}

// slow writes KTX2; fast skips uv and encodes AVIF at a low effort.
TEST_CASE(texture_format_and_fast_preset) {
    Settings settings;
    CHECK(run(texturedScan(), settings) ==
          (std::vector<std::string>{"normals", "weld", "dedup", "simplify", "uv", "ktx", "compress"}));

    settings.preset = optimize::Preset::fast;
    Recorder fast(texturedScan());
    optimize::plan(fast, settings, plenty);
    CHECK(fast.steps == (std::vector<std::string>{"normals", "weld", "dedup", "simplify", "avif", "compress"}));
    std::vector<std::string> expected = textureOptions("High", "4096");
    expected.insert(expected.end(), {"--effort", "2"});
    CHECK(*optionsOf(fast, "avif") == expected);
}

// The fast preset plans about 6 s for AVIF: images get smaller until their
// encoding fits. simplify runs whatever its time.
TEST_CASE(fast_preset_bounds_time) {
    Settings settings;
    settings.preset = optimize::Preset::fast;
    Model many = texturedScan();
    many.images.assign(16, image(4096, true));
    Recorder textures(many);
    optimize::plan(textures, settings, plenty);
    // 16 × 1536² is 38 megatexels, 4.9 s; 16 × 1792² would be 6.7 s.
    std::vector<std::string> expected = textureOptions("High", "1536");
    expected.insert(expected.end(), {"--effort", "2"});
    CHECK(*optionsOf(textures, "avif") == expected);

    Model dense = texturedScan();
    dense.faces = 10000000;
    Recorder geometry(dense);
    optimize::plan(geometry, settings, plenty);
    CHECK(geometry.steps[3] == "simplify");
}

// Textures fit 256 MB of GPU memory with the geometry, by one maximum side
// for the whole model, down to 1024; past that, a warning.
TEST_CASE(gpu_budget_sets_the_texture_side) {
    Model heavy = texturedScan();
    heavy.images.clear();
    for (int i = 0; i < 16; ++i) {
        heavy.images.push_back(image(4096, true));
        heavy.images.push_back(image(4096, false));
    }
    // 536 MB at 4096; 229 MB with the geometry at 2560, 273 MB at 2816.
    Recorder fitted(heavy);
    optimize::plan(fitted, Settings{}, plenty);
    CHECK(*optionsOf(fitted, "ktx") == textureOptions("High", "2560"));
    CHECK(fitted.warnings.empty());

    // 44 M faces at Highest: 880 MB of geometry alone.
    Model soup = heavy;
    soup.faces = 44000000;
    soup.vertices = 22000000;
    Settings highest;
    highest.quality = optimize::Quality::highest;
    Recorder over(soup);
    optimize::plan(over, highest, plenty);
    CHECK(*optionsOf(over, "ktx") == textureOptions("Highest", "1024"));
    CHECK(over.warnings == std::vector<std::string>{"gpu-budget-exceeded"});
}

// Images of 512 and less, and KTX2 ones within the side, are left as they
// are: without others, there's no texture step.
TEST_CASE(small_and_ktx2_images_have_no_texture_step) {
    Model small = texturedScan();
    small.images = {image(512, true), image(256, false)};
    CHECK(run(small, Settings{}) == (std::vector<std::string>{"normals", "weld", "dedup", "simplify", "uv", "compress"}));

    Model ktx2 = texturedScan();
    for (optimize::Image& image : ktx2.images) {
        image.width = image.height = 2048;
        image.ktx2 = true;
        image.gpu_bytes_per_texel = image.color ? 0.5 : 1;
    }
    Recorder recorder(ktx2);
    optimize::plan(recorder, Settings{}, plenty);
    CHECK(recorder.steps.size() == 6);
    CHECK(optionsOf(recorder, "uv") != nullptr && optionsOf(recorder, "uv")->empty());
}

// KTX2 images over the texture step's side are encoded again at it, and uv
// gets the same side to repack them first (docs/images.md#formats): 103 ETC1S
// images of 4096, as object-a has, go to the budget's side.
TEST_CASE(ktx2_images_over_the_side_are_encoded_again) {
    Model model = texturedScan();
    model.faces = 450000;
    model.vertices = 225000;
    model.images.clear();
    for (int i = 0; i < 103; ++i) {
        optimize::Image etc1s = image(4096, true);
        etc1s.file = std::to_string(i) + ".ktx2";
        etc1s.ktx2 = true;
        etc1s.gpu_bytes_per_texel = 0.5;
        model.images.push_back(etc1s);
    }
    model.occupancy = 0.47;
    Recorder recorder(model);
    optimize::plan(recorder, Settings{}, plenty);
    CHECK(recorder.steps == (std::vector<std::string>{"normals", "weld", "dedup", "simplify", "uv", "ktx", "compress"}));
    const std::vector<std::string>* ktx = optionsOf(recorder, "ktx");
    CHECK(ktx != nullptr && *ktx == textureOptions("High", "1792"));
    CHECK(*optionsOf(recorder, "uv") == (std::vector<std::string>{"--max-size", "1792"}));
    CHECK(recorder.warnings.empty());
}

// Small images stay as they are unless there are so many that the model
// misses the budget without encoding them: then ktx encodes them too. avif
// doesn't, since AVIF uploads as RGBA8 as well.
TEST_CASE(many_small_images_are_encoded_for_the_budget) {
    Model tiles = texturedScan();
    // 640 × 512² in RGBA8: 853 MB.
    tiles.images.assign(640, image(512, true));
    Recorder slow(tiles);
    optimize::plan(slow, Settings{}, plenty);
    CHECK(*optionsOf(slow, "ktx") == textureOptions("High", "4096", "0"));
    CHECK(slow.warnings.empty());

    Settings fast;
    fast.preset = optimize::Preset::fast;
    CHECK(run(tiles, fast) == (std::vector<std::string>{"normals", "weld", "dedup", "simplify", "compress"}));

    // A few small images stay as they are.
    tiles.images.resize(8);
    CHECK(run(tiles, Settings{}) == (std::vector<std::string>{"normals", "weld", "dedup", "simplify", "uv", "compress"}));
}

// Draco or meshopt geometry isn't simplified, unless normals rewrote it.
TEST_CASE(compressed_geometry_is_kept) {
    Model compressed = texturedScan();
    compressed.compressed_primitives = 1;
    CHECK(run(compressed, Settings{}) == (std::vector<std::string>{"normals", "weld", "dedup", "uv", "ktx", "compress"}));

    Model mixed = compressed;
    mixed.primitives = 2;
    CHECK(run(mixed, Settings{})[3] == "simplify");
}

// uv runs only when less than 0.6 of the texels are sampled, occupancy is
// known, and its xatlas data fits in half the memory; simplify's output
// counts, not import's.
TEST_CASE(uv_needs_waste_and_memory) {
    Model packed = texturedScan();
    packed.occupancy = 0.65;
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
    untextured.images.clear();
    untextured.occupancy.reset();
    CHECK(run(untextured, Settings{}) ==
          (std::vector<std::string>{"normals", "weld", "dedup", "simplify", "compress"}));

    Model images_only;
    images_only.images = {image(1024, true)};
    CHECK(run(images_only, Settings{}) == (std::vector<std::string>{"dedup", "ktx"}));
}

// The guard keeps the input when the output is larger and saves less than a
// tenth of the GPU memory.
TEST_CASE(guard_keeps_larger_outputs_only_for_a_gpu_gain) {
    CHECK(optimize::keepsInput(100, 120, 50, 50));
    CHECK(optimize::keepsInput(100, 120, 50, 46));
    CHECK(!optimize::keepsInput(100, 120, 50, 44));
    CHECK(!optimize::keepsInput(100, 90, 50, 50));
    CHECK(!optimize::keepsInput(0, 90, 50, 50));
}
