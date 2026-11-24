#include "meshtinker/cli/command.hpp"

#include "meshtinker/common/cleanup.hpp"
#include "meshtinker/common/failure.hpp"
#include "meshtinker/common/log.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <utility>

namespace meshtinker::cli {

const char* version() { return MESHTINKER_VERSION; }

Command::Command(std::string name, std::string summary, const std::string& description, Kind kind)
    : name_(std::move(name)), summary_(std::move(summary)), kind_(kind),
      app_(description.empty() ? summary_ : summary_ + "\n\n" + description, "meshtinker " + name_) {
    app_.set_version_flag("--version", std::string("meshtinker ") + version(), "Print the version and exit");
    // Help is the same whatever the terminal, so README blocks can be checked against it.
    app_.get_formatter()->column_width(32);
}

namespace {

bool hasExtension(const std::string& value, const char* extension) {
    return std::filesystem::u8path(value).extension() == extension;
}

common::Failure usageFailure(const std::string& message) {
    return common::Failure(common::ExitCode::usage, "usage", message);
}

// The value of `option` in `argv`, as `option value` or `option=value`, read
// before CLI11 parses the command line: a command line it rejects is reported
// in the format asked for. The last one given wins, as in CLI11.
std::optional<std::string> scanOption(int argc, char** argv, const std::string& option) {
    std::optional<std::string> value;
    const std::string prefix = option + "=";
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == option && i + 1 < argc) {
            value = argv[i + 1];
        } else if (argument.rfind(prefix, 0) == 0) {
            value = argument.substr(prefix.size());
        }
    }
    return value;
}

} // namespace

void Command::addInput() {
    const char* help = "The .gltf or .glb file";
    switch (kind_) {
    case Kind::transform:
    case Kind::read_only:
        help = "The .gltf or .glb file, or a chain: its last stage";
        break;
    case Kind::terminal:
        help = "The chain: its last stage";
        break;
    case Kind::files:
    case Kind::constructor:
        break;
    }
    app_.add_option("input", input_, help)->required();
}

void Command::addOutput(OutputFormats formats) {
    std::filesystem::path& output = output_.emplace();
    const bool glb = kind_ == Kind::terminal || (kind_ == Kind::files && formats == OutputFormats::gltf_or_glb);
    if (glb) {
        output_option_ = app_.add_option("output", output,
                                         "The .gltf or .glb file to write. Its directory must exist; no file is overwritten")
                             ->required()
                             ->check([](const std::string& value) {
                                 return hasExtension(value, ".gltf") || hasExtension(value, ".glb")
                                            ? std::string()
                                            : std::string("OUTPUT must end in .gltf or .glb");
                             });
        return;
    }
    switch (kind_) {
    case Kind::files:
    case Kind::read_only:
    case Kind::terminal: // handled above

        output_option_ = app_.add_option("output", output,
                                         "The .gltf file to write. Its directory must exist; no file is overwritten")
                             ->required()
                             ->check([](const std::string& value) {
                                 return hasExtension(value, ".gltf") ? std::string() : std::string("OUTPUT must end in .gltf");
                             });
        break;
    case Kind::transform:
        output_option_ = app_.add_option("output", output,
                                         "The .gltf file to write. Its directory must exist; no file is overwritten. "
                                         "Not given when INPUT is a chain: the command adds a stage to it")
                             ->check([](const std::string& value) {
                                 return hasExtension(value, ".gltf") ? std::string() : std::string("OUTPUT must end in .gltf");
                             });
        break;
    case Kind::constructor:
        output_option_ = app_.add_option("output", output,
                                         "The .gltf file to write, or a chain to create: a directory, new or empty. "
                                         "Its parent must exist; no file is overwritten")
                             ->required()
                             ->check([](const std::string& value) {
                                 return hasExtension(value, ".glb")
                                            ? std::string("OUTPUT must be a .gltf file or a directory")
                                            : std::string();
                             });
        break;
    }
}

void Command::addFlag(const std::string& name, bool& value, const std::string& help, bool hidden) {
    CLI::Option* option = app_.add_flag(name, value, help);
    if (hidden) {
        option->group("");
    }
}

void Command::addHiddenOption(const std::string& name, std::uint32_t& value) { app_.add_option(name, value)->group(""); }

void Command::addHiddenOption(const std::string& name, double& value) { app_.add_option(name, value)->group(""); }

void Command::addJsonOption() {
    app_.add_flag("--json", json_, "Print the result as one JSON object on stdout, even on failure");
}

void Command::addTrustedOption() {
    app_.add_flag("--trusted", trusted_,
                  "Let the model reference files outside its folder: absolute paths, `..` leading out, symlinks");
}

void Command::addWorkspaceOptions() {
    std::string default_location = output_ ? "the output's directory" : "the input's directory";
    if (kind_ != Kind::files) {
        default_location = "the chain, else " + default_location;
    }
    app_.add_option("--tmp-dir", tmp_dir_, "Where to create the temporary workspace (default: " + default_location + ")")
        ->check(CLI::ExistingDirectory);
    app_.add_flag("--keep-tmp", keep_tmp_, "Keep the temporary workspace and print its path, for debugging");
}

bool Command::hasOutput() const { return output_option_ != nullptr && output_option_->count() > 0; }

void Command::resolve() {
    resolved_input_ = input_;
    const bool input_is_chain = kind_ != Kind::files && kind_ != Kind::constructor && chain::isChain(input_);
    std::error_code error;
    if (kind_ == Kind::constructor && output_ && hasOutput() && !hasExtension(output_->u8string(), ".gltf")) {
        stage_ = chain::StageWriter::create(*output_, input_.filename().u8string(), name_, outputs_);
        return;
    }
    if (!input_is_chain) {
        if (kind_ != Kind::files && !std::filesystem::exists(input_, error)) {
            // A terminal command's input is a chain, which an earlier command
            // made: a missing one is the caller's problem, not the model's.
            throw common::systemFailure(ENOENT, kind_ == Kind::terminal ? common::Side::machine : common::Side::input,
                                        "file-unreadable",
                                        "cannot open '" + input_.filename().u8string() + "'");
        }
        if (kind_ == Kind::terminal) {
            throw usageFailure("INPUT must be a chain, which `meshtinker import` creates");
        }
        if (kind_ != Kind::files && std::filesystem::is_directory(input_, error)) {
            throw common::Failure(common::ExitCode::input_rejected, "not-a-chain",
                                  "'" + input_.filename().u8string() + "' is a directory but not a chain: it has no " +
                                      chain::chain_file);
        }
        if (kind_ == Kind::transform && !hasOutput()) {
            throw usageFailure("OUTPUT is required unless INPUT is a chain");
        }
        return;
    }
    if (kind_ == Kind::transform) {
        if (hasOutput()) {
            throw usageFailure("INPUT is a chain: leave OUTPUT out, the command adds a stage to it");
        }
        stage_ = chain::StageWriter::add(chain::Chain::open(input_), name_, outputs_);
        resolved_input_ = *stage_->parentModel();
        return;
    }
    chain_ = chain::Chain::open(input_);
    resolved_input_ = chain_->model(chain_->last());
}

std::filesystem::path Command::outputDirectory() const {
    if (stage_) {
        return stage_->directory();
    }
    if (!output_) {
        throw std::logic_error("outputDirectory() without addOutput()");
    }
    std::filesystem::path directory = output_->parent_path();
    if (directory.empty()) {
        directory = ".";
    }
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) {
        throw common::Failure(common::ExitCode::machine, "output-directory-missing",
                              "the output's directory doesn't exist");
    }
    return directory;
}

std::string Command::outputStem() const {
    if (stage_) {
        return stage_->stem();
    }
    if (!output_) {
        throw std::logic_error("outputStem() without addOutput()");
    }
    return output_->stem().u8string();
}

common::Workspace Command::workspace() const {
    if (tmp_dir_) {
        return common::Workspace(*tmp_dir_, keep_tmp_);
    }
    if (stage_) {
        return common::Workspace(stage_->chainDirectory(), keep_tmp_);
    }
    if (chain_) {
        return common::Workspace(chain_->directory(), keep_tmp_);
    }
    return common::Workspace(output_ ? outputDirectory() : input_.parent_path(), keep_tmp_);
}

void Command::publish(const nlohmann::ordered_json& report) {
    if (stage_) {
        stage_->finish(report, program() + " " + version(), argv_);
    }
    outputs_.commit();
    if (stage_) {
        stage_->published();
    }
}

std::vector<std::string> Command::logArguments() const {
    std::vector<std::string> arguments;
    if (log_level_ != common::LogLevel::info) {
        arguments.insert(arguments.end(), {"--log-level", common::logLevelName(log_level_)});
    }
    if (log_format_ != common::LogFormat::text) {
        arguments.insert(arguments.end(), {"--log-format", "json"});
    }
    return arguments;
}

nlohmann::ordered_json Command::jsonHeader(int format) const {
    return {{"format", format}, {"tool", {{"name", program()}, {"version", version()}}}};
}

void Command::addLogOptions() {
    // Names, checked here and converted once parsed: CLI11's message for a
    // wrong value is clearer than with a transformer.
    app_.add_option("--log-level", log_level_name_, "Print stderr messages of this level and above (default info)")
        ->check(CLI::IsMember({"trace", "debug", "info", "warn", "error"}))
        ->option_text("<trace|debug|info|warn|error>");
    app_.add_option("--log-format", log_format_name_, "Write stderr messages as text, or as JSON lines (default text)")
        ->check(CLI::IsMember({"text", "json"}))
        ->option_text("<text|json>");
}

void Command::reportFailure(const common::FailureReport& failure, bool json_requested) {
    common::logger().error(failure.message, failure.code);
    diagnostics_.log();
    if (json_requested) {
        nlohmann::ordered_json report = jsonHeader();
        report["error"] = {{"code", failure.code},
                           {"exitCode", common::toInt(failure.exit_code)},
                           {"message", failure.message}};
        report["diagnostics"] = diagnostics_.toJson();
        std::cout << report.dump(2) << '\n' << std::flush;
    }
}

int Command::run(int argc, char** argv, const std::function<int()>& body) {
    common::installInterruptCleanup();
    // Declared here, after the command's own options, so that they end --help.
    addLogOptions();
    if (argc == 2 && std::strcmp(argv[1], "--summary") == 0) {
        std::cout << summary_ << '\n';
        return 0;
    }
    // --json and the log options must hold even when the command line itself
    // is wrong. An invalid value keeps the default, and the parse error says so.
    bool json_requested = false;
    for (int i = 1; i < argc; ++i) {
        json_requested = json_requested || std::strcmp(argv[i], "--json") == 0;
    }
    const std::optional<std::string> level = scanOption(argc, argv, "--log-level");
    const std::optional<std::string> format = scanOption(argc, argv, "--log-format");
    common::logger().configure(program(), common::parseLogLevel(level.value_or("")).value_or(log_level_),
                               common::parseLogFormat(format.value_or("")).value_or(log_format_));
    try {
        app_.parse(argc, argv);
    } catch (const CLI::CallForHelp&) {
        std::cout << app_.help();
        return 0;
    } catch (const CLI::CallForVersion& version_call) {
        std::cout << version_call.what() << '\n';
        return 0;
    } catch (const CLI::ParseError& error) {
        // CLI11 starts some messages with the command's name, which reportFailure adds already.
        std::string message = error.what();
        const std::string prefix = app_.get_name() + ": ";
        if (message.rfind(prefix, 0) == 0) {
            message.erase(0, prefix.size());
        }
        reportFailure({common::ExitCode::usage, "usage", message}, json_requested);
        common::logger().info("Run '" + app_.get_name() + " --help' for usage.");
        return common::toInt(common::ExitCode::usage);
    }
    log_level_ = common::parseLogLevel(log_level_name_).value_or(common::LogLevel::info);
    log_format_ = common::parseLogFormat(log_format_name_).value_or(common::LogFormat::text);
    common::logger().configure(program(), log_level_, log_format_);
    argv_.assign(argv, argv + argc);
    try {
        resolve();
        const int code = body();
        diagnostics_.log();
        return code;
    } catch (...) {
        // A command that deferred interruptions unwinds first, so that its
        // destructors clean up, then dies of the signal.
        if (common::interruption() != 0) {
            common::dieOfInterruption();
        }
        const common::FailureReport failure = common::describeFailure(std::current_exception());
        reportFailure(failure, json_);
        if (failure.exit_code == common::ExitCode::usage) {
            common::logger().info("Run '" + app_.get_name() + " --help' for usage.");
        }
        return common::toInt(failure.exit_code);
    }
}

} // namespace meshtinker::cli
