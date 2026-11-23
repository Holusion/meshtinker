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

struct Settings {
    Preset preset = Preset::slow;
    Quality quality = Quality::high;
};

// An image as the plan sees it.
struct Image {
    std::string file;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // Every slot using it holds color (base color, emissive): ktx encodes it
    // as ETC1S, and anything else as UASTC (docs/ktx.md).
    bool color = false;
    // Already KTX2, which ktx and avif leave as it is.
    bool ktx2 = false;
    // Bytes a texel takes on the GPU as the image is now, without mipmaps:
    // 4 for PNG, JPEG, WebP and AVIF (RGBA8), 0.5 or 1 for KTX2.
    double gpu_bytes_per_texel = 4;

    std::uint32_t longestSide() const { return width > height ? width : height; }
};

// What the plan knows about the model: import's report, updated by the steps
// that change it.
struct Model {
    std::uint64_t primitives = 0;
    // Primitives in Draco or meshopt, which only simplify would decompress.
    std::uint64_t compressed_primitives = 0;
    std::uint64_t vertices = 0;
    std::uint64_t faces = 0;
    std::vector<Image> images;
    // The fraction of texels the model samples, if import could measure it.
    std::optional<double> occupancy;

    bool hasGeometry() const { return faces > 0; }
    bool hasTextures() const { return !images.empty(); }
    bool geometryIsCompressed() const { return primitives > 0 && compressed_primitives == primitives; }
};

// Estimated GPU memory, in bytes, of the model's geometry once compressed
// (16 bytes a quantized vertex, 12 a face), and of its textures as they are
// now, with mipmaps (docs/optimize.md#gpu-budget).
double geometryGpuBytes(const Model& model);
double textureGpuBytes(const Model& model);

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
    // A warning from the plan itself (diagnostics, with a stable code).
    void warn(const std::string& code, const std::string& message) { warned(code, message); }

protected:
    virtual void execute(const std::string& command, const std::vector<std::string>& options) = 0;
    virtual void warned(const std::string& code, const std::string& message) = 0;

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
    // The child died of this signal (0: it exited).
    int signal = 0;
    double seconds = 0;
    nlohmann::ordered_json report;
};

struct Report {
    Settings settings;
    Machine machine;
    std::vector<StepRecord> steps;
    std::uint64_t input_bytes = 0;
    std::uint64_t output_bytes = 0;
    // The stage exported: the last, or import's when the guard kept the
    // input (docs/optimize.md#guard).
    std::string exported_stage;
    double input_gpu_bytes = 0;
    double output_gpu_bytes = 0;
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
                const std::filesystem::path& workspace, const Options& options, common::Diagnostics& diagnostics);

// Whether to export import's stage rather than the last: the output would be
// larger than the input, for little or no GPU memory saved.
bool keepsInput(std::uint64_t input_bytes, std::uint64_t output_bytes, double input_gpu_bytes, double output_gpu_bytes);

nlohmann::ordered_json toJson(const Report& report, nlohmann::ordered_json header, const common::Diagnostics& diagnostics);
void printText(std::ostream& out, const Report& report);

} // namespace meshtinker::optimize
