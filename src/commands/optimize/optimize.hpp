#pragma once

#include "meshtinker/common/diagnostics.hpp"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace meshtinker::optimize {

// eCorpus's quality names, as `ktx` and `avif` take them.
enum class Quality : std::uint8_t { highest, high, medium, low, thumb };
const char* qualityName(Quality quality);

// `slow` is the asset to publish; `fast` a temporary model made while it is
// being worked on (docs/optimize.md#presets).
enum class Preset : std::uint8_t { slow, fast };
const char* presetName(Preset preset);

// The texture format: KTX2 stays compressed on the GPU, AVIF downloads smaller.
enum class Textures : std::uint8_t { ktx, avif };
const char* texturesName(Textures textures);

struct Settings {
    Preset preset = Preset::slow;
    Quality quality = Quality::high;
    Textures textures = Textures::ktx;
};

// What the plan knows about the model: import's report, updated by the steps
// that change it.
struct Model {
    std::uint64_t primitives = 0;
    std::uint64_t faces = 0;
    std::uint64_t images = 0;
    // The longest side of the largest image.
    std::uint32_t largest_image = 0;
    // The fraction of texels the model samples, if import could measure it.
    std::optional<double> occupancy;

    bool hasGeometry() const { return faces > 0; }
    bool hasTextures() const { return images > 0; }
};

// What the plan knows about the machine.
struct Machine {
    // Bytes of memory the steps may use: the cgroup's limit or the system's
    // available memory, whichever is lower.
    std::uint64_t memory = 0;
};

// The machine optimize runs on.
Machine thisMachine();

// The steps a plan runs, on a chain that `import` created; `export` writes
// the last stage once the plan returns. `steps("weld")` runs `weld` on the
// chain at once and updates model(), so a condition after it sees its result.
class Steps {
public:
    virtual ~Steps() = default;
    const Model& model() const { return model_; }
    void operator()(const std::string& command, const std::vector<std::string>& options = {}) {
        execute(command, options);
    }

protected:
    virtual void execute(const std::string& command, const std::vector<std::string>& options) = 0;

    Model model_;
};

// Runs the steps that suit `model` for `settings`, in the step order of
// docs/chaining.md (plan.cpp, docs/optimize.md#plan).
void plan(Steps& run, const Settings& settings, const Machine& machine);

// A step as it ran.
struct StepRecord {
    std::string command;
    std::vector<std::string> arguments;
    int exit_code = 0;
    double seconds = 0;
    nlohmann::ordered_json report;
};

struct Report {
    Settings settings;
    Machine machine;
    std::vector<StepRecord> steps;
    std::uint64_t input_bytes = 0;
    std::uint64_t output_bytes = 0;
    double seconds = 0;
};

struct Options {
    Settings settings;
    bool trusted = false;
    // Where the `meshtinker-<name>` executables are looked for first.
    std::filesystem::path own_directory;
};

// Imports `input` into a chain in `workspace`, runs plan() on it, and exports
// the last stage to `output` (docs/optimize.md).
Report optimize(const std::filesystem::path& input, const std::filesystem::path& output,
                const std::filesystem::path& workspace, const Options& options);

nlohmann::ordered_json toJson(const Report& report, nlohmann::ordered_json header, const common::Diagnostics& diagnostics);
void printText(std::ostream& out, const Report& report);

} // namespace meshtinker::optimize
