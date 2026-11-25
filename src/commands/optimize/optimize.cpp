#include "optimize.hpp"

#include "meshtinker/common/cleanup.hpp"
#include "meshtinker/common/exit_code.hpp"
#include "meshtinker/common/failure.hpp"
#include "meshtinker/common/format.hpp"
#include "meshtinker/common/log.hpp"
#include "meshtinker/common/process.hpp"
#include "meshtinker/common/subcommands.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <limits>

namespace meshtinker::optimize {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::ordered_json;
using common::ExitCode;
using common::Failure;

// A cgroup v2 file holding one number, or "max".
std::optional<std::uint64_t> readCgroupValue(const fs::path& path) {
    std::ifstream in(path);
    std::string text;
    if (!(in >> text) || text == "max") {
        return std::nullopt;
    }
    try {
        return std::stoull(text);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// MemAvailable in /proc/meminfo, in bytes.
std::optional<std::uint64_t> availableMemory() {
    std::ifstream in("/proc/meminfo");
    std::string key;
    std::uint64_t kib = 0;
    std::string unit;
    while (in >> key >> kib >> unit) {
        if (key == "MemAvailable:") {
            return kib * 1024;
        }
    }
    return std::nullopt;
}

ExitCode exitCodeOf(int code) {
    for (const ExitCode known : {ExitCode::input_rejected, ExitCode::usage, ExitCode::resource_limit,
                                 ExitCode::machine, ExitCode::internal}) {
        if (common::toInt(known) == code) {
            return known;
        }
    }
    return ExitCode::internal;
}

std::uint64_t unsignedOf(const Json& object, const char* key, std::uint64_t fallback = 0) {
    return object.is_object() && object.contains(key) && object[key].is_number_unsigned() ? object[key].get<std::uint64_t>()
                                                                                           : fallback;
}

std::string stringOf(const Json& object, const char* key) {
    return object.is_object() && object.contains(key) && object[key].is_string() ? object[key].get<std::string>()
                                                                                  : std::string();
}

std::uint32_t sideOf(const Json& object, const char* key) {
    const std::uint64_t side = unsignedOf(object, key);
    return side > std::numeric_limits<std::uint32_t>::max() ? std::numeric_limits<std::uint32_t>::max()
                                                            : static_cast<std::uint32_t>(side);
}

// Slots that hold color, which ktx encodes as ETC1S (docs/ktx.md).
bool colorSlot(const std::string& slot) {
    return slot == "baseColorTexture" || slot == "emissiveTexture" || slot == "diffuseTexture";
}

// The files of a step's output, in bytes.
std::uint64_t outputBytes(const Json& report) {
    std::uint64_t bytes = 0;
    const Json output = report.is_object() ? report.value("output", Json::object()) : Json::object();
    for (const Json& file : output.value("files", Json::array())) {
        bytes += unsignedOf(file, "byteSize");
    }
    return bytes;
}

// The steps, run as child processes on the chain.
class ChildSteps : public Steps {
public:
    ChildSteps(fs::path chain, const Options& options, Report& report, common::Diagnostics& diagnostics)
        : chain_(std::move(chain)), options_(options), report_(report), diagnostics_(diagnostics) {}

    // The last stage's files, in bytes, as the last step that succeeded
    // reported them; 0 while there is only import's.
    std::uint64_t lastStageBytes() const { return last_stage_bytes_; }

    void execute(const std::string& command, const std::vector<std::string>& options) override {
        runStep(command, options, {chain_.string()}, false);
    }

    void warned(const std::string& code, const std::string& message) override { diagnostics_.warn(code, message); }

    // `meshtinker-<command> --json [--trusted] [log options] options...
    // targets...`, then what its report says about the model. Each step logs
    // as it starts and ends, so that a caller reading stderr sees which runs. A step that fails or dies leaves
    // the chain as it was: unless it is `required`, optimize warns and goes on
    // from the stage before it.
    void runStep(const std::string& command, const std::vector<std::string>& options,
                 const std::vector<std::string>& targets, bool required) {
        // A signal between two steps stops before the next one.
        if (common::interruption() != 0) {
            throw Failure(ExitCode::internal, "interrupted", "Interrupted before " + command);
        }
        const std::optional<fs::path> program = common::findCommand(options_.own_directory, command);
        if (!program) {
            throw Failure(ExitCode::machine, "command-missing",
                          "meshtinker-" + command + " isn't next to meshtinker-optimize or on PATH");
        }
        std::vector<std::string> arguments = {"meshtinker-" + command, "--json"};
        if (options_.trusted && command == "import") {
            arguments.emplace_back("--trusted");
        }
        arguments.insert(arguments.end(), options_.log_arguments.begin(), options_.log_arguments.end());
        arguments.insert(arguments.end(), options.begin(), options.end());
        arguments.insert(arguments.end(), targets.begin(), targets.end());

        common::Log& log = common::logger();
        log.info("Running " + command);
        if (log.enabled(common::LogLevel::debug)) {
            // The options, without the targets: messages name no folders.
            std::string line = arguments.front();
            for (std::size_t i = 1; i < arguments.size() - targets.size(); ++i) {
                line += " " + arguments[i];
            }
            log.debug(line);
        }
        const auto start = std::chrono::steady_clock::now();
        const common::ChildResult result = common::runChild(*program, arguments);
        StepRecord step;
        step.command = command;
        step.arguments.assign(arguments.begin() + 1, arguments.end());
        step.exit_code = result.exit_code;
        step.signal = result.signal;
        step.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const bool failed = result.signal != 0 || result.exit_code != 0;
        log.info(command + (failed ? " failed after " : " took ") + common::formatDuration(step.seconds));
        step.report = Json::parse(result.output, nullptr, false);
        if (step.report.is_discarded()) {
            step.report = Json();
        }
        report_.steps.push_back(step);

        if (common::interruption() != 0) {
            throw Failure(ExitCode::internal, "interrupted", "Interrupted during " + command);
        }
        if (failed) {
            const Json error = step.report.is_object() ? step.report.value("error", Json::object()) : Json::object();
            const std::string what =
                result.signal != 0 ? "was killed by signal " + std::to_string(result.signal)
                                   : "failed: " + error.value("message", std::string("exit code ") +
                                                                            std::to_string(result.exit_code));
            if (required) {
                throw Failure(result.signal != 0 ? ExitCode::internal : exitCodeOf(result.exit_code),
                              result.signal != 0 ? std::string("step-killed")
                                                 : error.value("code", std::string("step-failed")),
                              "meshtinker-" + command + " " + what);
            }
            diagnostics_.warn("step-failed",
                              "steps that fail are left out, and the next one goes on from the stage before",
                              "meshtinker-" + command + " " + what);
            return;
        }
        update(command, step.report);
    }

private:
    Image* imageNamed(const std::string& file) {
        for (Image& image : model_.images) {
            if (image.file == file) {
                return &image;
            }
        }
        return nullptr;
    }

    // An image a step wrote in place of `source`. uv lists the images of
    // groups it leaves as they are too, without an output.
    void replaced(const Json& entry, bool ktx2, double gpu_bytes_per_texel) {
        Image* image = imageNamed(stringOf(entry, "source"));
        if (image == nullptr || !entry.contains("output")) {
            return;
        }
        image->file = stringOf(entry, "output");
        image->width = sideOf(entry, "outputWidth");
        image->height = sideOf(entry, "outputHeight");
        image->ktx2 = ktx2;
        image->gpu_bytes_per_texel = gpu_bytes_per_texel;
    }

    void counts(const Json& output) {
        model_.primitives = unsignedOf(output, "primitives", model_.primitives);
        model_.vertices = unsignedOf(output, "vertices", model_.vertices);
        model_.faces = unsignedOf(output, "faces", model_.faces);
    }

    // What a step's report says about the model, for later conditions.
    void update(const std::string& command, const Json& report) {
        if (command != "import") {
            last_stage_bytes_ = outputBytes(report);
        }
        if (command == "import") {
            const Json& geometry = report.value("geometry", Json::object());
            counts(geometry);
            const Json encodings = geometry.value("primitivesByEncoding", Json::object());
            model_.compressed_primitives = unsignedOf(encodings, "draco") + unsignedOf(encodings, "meshopt");
            for (const Json& entry : report.value("images", Json::array())) {
                Image image;
                image.file = stringOf(entry, "file");
                image.width = sideOf(entry, "width");
                image.height = sideOf(entry, "height");
                image.ktx2 = stringOf(entry, "mimeType") == "image/ktx2";
                image.gpu_bytes_per_texel = image.ktx2 ? 1 : 4;
                const Json uses = entry.value("uses", Json::array());
                image.color = !uses.empty() && std::all_of(uses.begin(), uses.end(), [](const Json& use) {
                    return colorSlot(stringOf(use, "slot"));
                });
                model_.images.push_back(image);
            }
            const Json texels = report.value("texels", Json::object());
            if (texels.contains("occupancy") && texels["occupancy"].is_number()) {
                model_.occupancy = texels["occupancy"].get<double>();
            }
            report_.input_bytes = report.value("input", Json::object()).value("byteSize", std::uint64_t{0});
        } else if (command == "normals") {
            const Json primitives = report.value("primitives", Json::object());
            const std::uint64_t rewritten = unsignedOf(primitives, "added") + unsignedOf(primitives, "smoothed");
            // Its input and output are the rewritten primitives only.
            const Json before = report.value("input", Json::object());
            const Json after = report.value("output", Json::object());
            const auto adjust = [&](std::uint64_t& count, const char* key) {
                count = count - std::min(count, unsignedOf(before, key)) + unsignedOf(after, key);
            };
            adjust(model_.vertices, "vertices");
            adjust(model_.faces, "faces");
            model_.compressed_primitives =
                std::min(model_.compressed_primitives, model_.primitives - std::min(model_.primitives, rewritten));
        } else if (command == "weld") {
            counts(report.value("output", Json::object()));
        } else if (command == "simplify") {
            counts(report.value("output", Json::object()));
            const std::uint64_t simplified = unsignedOf(report.value("primitives", Json::object()), "simplified");
            model_.compressed_primitives =
                std::min(model_.compressed_primitives, model_.primitives - std::min(model_.primitives, simplified));
        } else if (command == "uv") {
            for (const Json& group : report.value("groups", Json::array())) {
                for (const Json& entry : group.value("images", Json::array())) {
                    replaced(entry, false, 4);
                }
            }
        } else if (command == "ktx") {
            for (const Json& entry : report.value("images", Json::array())) {
                const bool uastc = stringOf(entry, "codec") == "UASTC";
                const bool alpha = entry.value("alpha", false);
                replaced(entry, true, uastc || alpha ? 1.0 : 0.5);
            }
        } else if (command == "avif") {
            for (const Json& entry : report.value("images", Json::array())) {
                replaced(entry, false, 4);
            }
        } else if (command == "compress") {
            model_.compressed_primitives = model_.primitives;
        }
    }

    fs::path chain_;
    const Options& options_;
    Report& report_;
    common::Diagnostics& diagnostics_;
    std::uint64_t last_stage_bytes_ = 0;
};

} // namespace

const char* qualityName(Quality quality) {
    switch (quality) {
    case Quality::highest:
        return "Highest";
    case Quality::high:
        return "High";
    case Quality::medium:
        return "Medium";
    case Quality::low:
        return "Low";
    case Quality::thumb:
        return "Thumb";
    }
    return "High";
}

const char* presetName(Preset preset) { return preset == Preset::fast ? "fast" : "slow"; }

Machine thisMachine() {
    Machine machine;
    std::uint64_t memory = availableMemory().value_or(std::numeric_limits<std::uint64_t>::max());
    // cgroup v2: the limit of the process's own group, less what it uses.
    std::ifstream group("/proc/self/cgroup");
    std::string line;
    while (std::getline(group, line)) {
        if (line.rfind("0::", 0) != 0) {
            continue;
        }
        const fs::path directory = fs::path("/sys/fs/cgroup") / fs::path(line.substr(3)).relative_path();
        const std::optional<std::uint64_t> limit = readCgroupValue(directory / "memory.max");
        const std::optional<std::uint64_t> used = readCgroupValue(directory / "memory.current");
        if (limit) {
            memory = std::min(memory, *limit - std::min(*limit, used.value_or(0)));
        }
    }
    machine.memory = memory == std::numeric_limits<std::uint64_t>::max() ? 0 : memory;
    return machine;
}

bool keepsInput(std::uint64_t input_bytes, std::uint64_t output_bytes, double input_gpu_bytes, double output_gpu_bytes) {
    // The output must save a tenth of the GPU memory to be worth more bytes.
    return input_bytes > 0 && output_bytes > input_bytes && output_gpu_bytes > 0.9 * input_gpu_bytes;
}

Report optimize(const fs::path& input, const fs::path& output, const fs::path& workspace, const Options& options,
                common::Diagnostics& diagnostics) {
    const auto start = std::chrono::steady_clock::now();
    Report report;
    report.settings = options.settings;
    report.machine = thisMachine();
    // Signals go to the running step, which cleans up after itself; optimize
    // then unwinds, and the workspace goes with the chain.
    common::deferInterruptions();
    const fs::path chain = workspace / "chain";
    ChildSteps steps(chain, options, report, diagnostics);
    steps.runStep("import", {}, {input.string(), chain.string()}, true);
    report.input_gpu_bytes = geometryGpuBytes(steps.model()) + textureGpuBytes(steps.model());
    plan(steps, options.settings, report.machine);
    report.output_gpu_bytes = geometryGpuBytes(steps.model()) + textureGpuBytes(steps.model());
    std::vector<std::string> export_options;
    if (keepsInput(report.input_bytes, steps.lastStageBytes(), report.input_gpu_bytes, report.output_gpu_bytes)) {
        export_options = {"--stage", "0"};
        report.output_gpu_bytes = report.input_gpu_bytes;
        diagnostics.warn("input-kept",
                         "the optimized model was larger than the input and saved less than a tenth of its GPU "
                         "memory: the input is written as import read it");
    }
    steps.runStep("export", export_options, {chain.string(), output.string()}, true);
    if (common::interruption() != 0) {
        throw Failure(ExitCode::internal, "interrupted", "Interrupted after export");
    }
    const Json& exported = report.steps.back().report;
    report.exported_stage = exported.value("stage", std::string());
    report.output_bytes = 0;
    for (const Json& file : exported.value("files", Json::array())) {
        report.output_bytes += file.value("byteSize", std::uint64_t{0});
    }
    report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return report;
}

} // namespace meshtinker::optimize
