#pragma once

#include "meshtinker/common/diagnostics.hpp"
#include "meshtinker/common/run.hpp"
#include "meshtinker/common/workspace.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace meshtinker::cli {

// The version every command reports.
const char* version();

// A subcommand's command line, declared once: its options and their help text
// go through CLI11, and `--help` is generated from them. The standard options
// below keep their names and wording the same in every command.
class Command {
public:
    // `name` without the `meshtinker-` prefix. `summary` is the one line the
    // wrapper lists; `description` opens `--help`, after the summary.
    Command(std::string name, std::string summary, const std::string& description = "");

    CLI::App& app() { return app_; }
    const std::string& name() const { return name_; }
    // `meshtinker-<name>`, as messages and reports name it.
    std::string program() const { return "meshtinker-" + name_; }
    common::Diagnostics& diagnostics() { return diagnostics_; }

    // The `input` positional: the .gltf or .glb file the command reads.
    void addInput();
    // The `output` positional: the .gltf file the command writes, with its
    // .bin and images beside it. Declare it after the input.
    void addOutput();
    // --json: print the result as one JSON object on stdout, even on failure.
    void addJsonOption();
    // --trusted: let the model reference files outside its folder.
    void addTrustedOption();
    // --tmp-dir and --keep-tmp. The workspace goes in the output's directory,
    // or the input's for a command without output: declare them after the
    // positionals.
    void addWorkspaceOptions();

    bool json() const { return json_; }
    bool trusted() const { return trusted_; }
    const std::filesystem::path& input() const { return input_; }

    // The output's directory, "." when the output names none. Throws a
    // Failure (`output-directory-missing`, exit code 4) if it doesn't exist,
    // so call it from run()'s body.
    std::filesystem::path outputDirectory() const;
    // The output's file name without `.gltf`: the stem of every file the
    // command writes.
    std::string outputStem() const;
    // The command's workspace: in --tmp-dir, or in the default location
    // (see addWorkspaceOptions). Created on first use.
    common::Workspace workspace() const;

    // The start of every JSON object the command prints: `format` (the
    // version of the command's report shape) and `tool`.
    nlohmann::ordered_json jsonHeader(int format = 1) const;

    // The whole life of a command: installs interruption cleanup, answers the
    // hidden `--summary`, parses the command line, runs `body` and reports.
    // `body` prints the command's output and returns its exit code.
    //
    // Command-line errors exit with 2. An exception from `body` is reported
    // through common::describeFailure. Diagnostics go to stderr, after the
    // error if there is one. With --json, a failure prints
    // `{format, tool, error: {code, exitCode, message}, diagnostics}` on stdout.
    int run(int argc, char** argv, const std::function<int()>& body);

private:
    void reportFailure(const common::FailureReport& failure, bool json_requested);

    std::string name_;
    std::string summary_;
    CLI::App app_;
    common::Diagnostics diagnostics_;
    std::filesystem::path input_;
    std::optional<std::filesystem::path> output_;
    bool json_ = false;
    bool trusted_ = false;
    std::optional<std::filesystem::path> tmp_dir_;
    bool keep_tmp_ = false;
};

} // namespace meshtinker::cli
