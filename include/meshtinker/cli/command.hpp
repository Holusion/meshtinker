#pragma once

#include "meshtinker/chain/chain.hpp"
#include "meshtinker/common/diagnostics.hpp"
#include "meshtinker/common/outputs.hpp"
#include "meshtinker/common/run.hpp"
#include "meshtinker/common/workspace.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace meshtinker::cli {

// The version every command reports.
const char* version();

// How a command takes part in chains (docs/chaining.md), which decides what
// its `input` and `output` positionals accept.
enum class Kind : std::uint8_t {
    // Reads and writes files only: `cmd in out.gltf` (placeholder), or
    // `cmd in out.glb` when the command writes GLB too (optimize).
    files,
    // `import in out.gltf`, or `import in work/` to create a chain and its
    // stage 000.
    constructor,
    // `cmd in out.gltf`, or `cmd work/` to add a stage built on the last one.
    transform,
    // `cmd in`, where `in` may be a chain: its last stage is read.
    read_only,
    // `cmd work/ out`: reads the last stage of a chain, and writes elsewhere.
    terminal,
};

// The files a command's output may be. Stages are always .gltf, so this
// matters only to the files kind; the terminal kind writes either.
enum class OutputFormats : std::uint8_t { gltf, gltf_or_glb };

// A subcommand's command line, declared once: its options and their help text
// go through CLI11, and `--help` is generated from them. The standard options
// below keep their names and wording the same in every command.
class Command {
public:
    // `name` without the `meshtinker-` prefix. `summary` is the one line the
    // wrapper lists; `description` opens `--help`, after the summary.
    Command(std::string name, std::string summary, const std::string& description = "", Kind kind = Kind::files);

    CLI::App& app() { return app_; }
    const std::string& name() const { return name_; }
    Kind kind() const { return kind_; }
    // `meshtinker-<name>`, as messages and reports name it.
    std::string program() const { return "meshtinker-" + name_; }
    common::Diagnostics& diagnostics() { return diagnostics_; }

    // The `input` positional: the .gltf or .glb file the command reads, or a
    // chain, depending on the kind.
    void addInput();
    // The `output` positional: the .gltf file the command writes, with its
    // .bin and images beside it, or what the kind accepts instead. Declare it
    // after the input.
    void addOutput(OutputFormats formats = OutputFormats::gltf);
    // A command's own flag. CLI11's add_flag is a template that can throw, so
    // calling it from main()'s translation unit makes clang-tidy's
    // exception-escape check fire; this keeps it in command.cpp. A hidden flag
    // is left out of --help, for measurements.
    void addFlag(const std::string& name, bool& value, const std::string& help, bool hidden = false);
    // Options left out of --help, for measurements.
    void addHiddenOption(const std::string& name, std::uint32_t& value);
    void addHiddenOption(const std::string& name, double& value);
    // --json: print the result as one JSON object on stdout, even on failure.
    void addJsonOption();
    // --trusted: let the model reference files outside its folder.
    void addTrustedOption();
    // --tmp-dir and --keep-tmp. The workspace goes in the chain's directory in
    // chain mode, else in the output's directory, or the input's for a
    // command without output: declare them after the positionals.
    void addWorkspaceOptions();

    bool json() const { return json_; }
    bool trusted() const { return trusted_; }
    // The model to read: the input as given, or the last stage's model when
    // the input is a chain.
    const std::filesystem::path& input() const { return resolved_input_; }
    // The input as given on the command line.
    const std::filesystem::path& inputArgument() const { return input_; }

    // Where the command writes its model: the pending stage's directory in
    // chain mode, else the output's directory, "." when the output names
    // none. Throws a Failure (`output-directory-missing`, exit code 4) if it
    // doesn't exist, so call it from run()'s body.
    std::filesystem::path outputDirectory() const;
    // The stem of every file the command writes: the source's in chain mode,
    // else the output's file name without its extension.
    std::string outputStem() const;
    // The output as given, for kinds that write elsewhere than a chain.
    const std::optional<std::filesystem::path>& outputArgument() const { return output_; }
    // The command's workspace: in --tmp-dir, or in the default location
    // (see addWorkspaceOptions). Created on first use.
    common::Workspace workspace() const;

    // The files the command writes. In chain mode they are written in the
    // pending stage, and publish() publishes them.
    common::Outputs& outputs() { return outputs_; }
    // The stage being written, in chain mode (constructor and transform
    // kinds), or null.
    chain::StageWriter* stage() { return stage_.get(); }
    // The chain read, for read-only and terminal kinds in chain mode, or null.
    const chain::Chain* chain() const { return chain_ ? &*chain_ : nullptr; }

    // Publishes what the command wrote. In chain mode, first writes
    // stage.json with `report`, the command's JSON report.
    void publish(const nlohmann::ordered_json& report);

    // The start of every JSON object the command prints: `format` (the
    // version of the command's report shape) and `tool`.
    nlohmann::ordered_json jsonHeader(int format = 1) const;

    // The whole life of a command: installs interruption cleanup, answers the
    // hidden `--summary`, parses the command line, resolves the positionals
    // (taking the chain's lock for a stage), runs `body` and reports.
    // `body` prints the command's output and returns its exit code.
    //
    // Command-line errors exit with 2, including a chain given an output it
    // doesn't take. An exception from `body` is reported through
    // common::describeFailure. Diagnostics go to stderr, after the error if
    // there is one. With --json, a failure prints
    // `{format, tool, error: {code, exitCode, message}, diagnostics}` on stdout.
    int run(int argc, char** argv, const std::function<int()>& body);

private:
    void reportFailure(const common::FailureReport& failure, bool json_requested);
    // Decides between file and chain mode once the command line is parsed.
    void resolve();
    bool hasOutput() const;

    std::string name_;
    std::string summary_;
    Kind kind_;
    CLI::App app_;
    common::Diagnostics diagnostics_;
    std::filesystem::path input_;
    std::filesystem::path resolved_input_;
    std::optional<std::filesystem::path> output_;
    CLI::Option* output_option_ = nullptr;
    bool json_ = false;
    bool trusted_ = false;
    std::optional<std::filesystem::path> tmp_dir_;
    bool keep_tmp_ = false;
    std::vector<std::string> argv_;
    std::optional<chain::Chain> chain_;
    // Declared before the outputs, so that they go first: a new chain's
    // directory is removed after its files.
    std::unique_ptr<chain::StageWriter> stage_;
    common::Outputs outputs_;
};

} // namespace meshtinker::cli
