#include "optimize.hpp"

#include "meshtinker/common/cleanup.hpp"
#include "meshtinker/common/exit_code.hpp"
#include "meshtinker/common/failure.hpp"
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

// The steps, run as child processes on the chain.
class ChildSteps : public Steps {
public:
    ChildSteps(fs::path chain, const Options& options, Report& report)
        : chain_(std::move(chain)), options_(options), report_(report) {}

    // `meshtinker-<command> --json [--trusted] options... targets...`, then
    // what its report says about the model.
    void execute(const std::string& command, const std::vector<std::string>& options) override {
        runStep(command, options, {chain_.string()});
    }

    void runStep(const std::string& command, const std::vector<std::string>& options,
                 const std::vector<std::string>& targets) {
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
        arguments.insert(arguments.end(), options.begin(), options.end());
        arguments.insert(arguments.end(), targets.begin(), targets.end());

        const auto start = std::chrono::steady_clock::now();
        const common::ChildResult result = common::runChild(*program, arguments);
        StepRecord step;
        step.command = command;
        step.arguments.assign(arguments.begin() + 1, arguments.end());
        step.exit_code = result.exit_code;
        step.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        step.report = Json::parse(result.output, nullptr, false);
        if (step.report.is_discarded()) {
            step.report = Json();
        }
        report_.steps.push_back(step);

        if (common::interruption() != 0) {
            throw Failure(ExitCode::internal, "interrupted", "Interrupted during " + command);
        }
        if (result.signal != 0) {
            throw Failure(ExitCode::internal, "step-killed",
                          "meshtinker-" + command + " was killed by signal " + std::to_string(result.signal));
        }
        if (result.exit_code != 0) {
            const Json error = step.report.is_object() ? step.report.value("error", Json::object()) : Json::object();
            throw Failure(exitCodeOf(result.exit_code), error.value("code", std::string("step-failed")),
                          "meshtinker-" + command + ": " + error.value("message", std::string("failed")));
        }
        update(command, step.report);
    }

private:
    // What a step's report says about the model, for later conditions.
    void update(const std::string& command, const Json& report) {
        if (command == "import") {
            const Json& geometry = report.value("geometry", Json::object());
            model_.primitives = geometry.value("primitives", std::uint64_t{0});
            model_.faces = geometry.value("faces", std::uint64_t{0});
            const Json images = report.value("images", Json::array());
            model_.images = images.size();
            for (const Json& image : images) {
                const auto side = [&](const char* key) {
                    return image.contains(key) && image[key].is_number_unsigned() ? image[key].get<std::uint32_t>() : 0U;
                };
                model_.largest_image = std::max({model_.largest_image, side("width"), side("height")});
            }
            const Json texels = report.value("texels", Json::object());
            if (texels.contains("occupancy") && texels["occupancy"].is_number()) {
                model_.occupancy = texels["occupancy"].get<double>();
            }
            report_.input_bytes = report.value("input", Json::object()).value("byteSize", std::uint64_t{0});
        } else if (command == "simplify") {
            model_.faces = report.value("output", Json::object()).value("faces", model_.faces);
        }
    }

    fs::path chain_;
    const Options& options_;
    Report& report_;
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

const char* texturesName(Textures textures) { return textures == Textures::avif ? "avif" : "ktx"; }

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

Report optimize(const fs::path& input, const fs::path& output, const fs::path& workspace, const Options& options) {
    const auto start = std::chrono::steady_clock::now();
    Report report;
    report.settings = options.settings;
    report.machine = thisMachine();
    // Signals go to the running step, which cleans up after itself; optimize
    // then unwinds, and the workspace goes with the chain.
    common::deferInterruptions();
    const fs::path chain = workspace / "chain";
    ChildSteps steps(chain, options, report);
    steps.runStep("import", {}, {input.string(), chain.string()});
    plan(steps, options.settings, report.machine);
    steps.runStep("export", {}, {chain.string(), output.string()});
    if (common::interruption() != 0) {
        throw Failure(ExitCode::internal, "interrupted", "Interrupted after export");
    }
    report.output_bytes = 0;
    for (const Json& file : report.steps.back().report.value("files", Json::array())) {
        report.output_bytes += file.value("byteSize", std::uint64_t{0});
    }
    report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return report;
}

} // namespace meshtinker::optimize
