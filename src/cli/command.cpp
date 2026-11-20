#include "meshtinker/cli/command.hpp"

#include "meshtinker/common/cleanup.hpp"
#include "meshtinker/common/failure.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
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

void Command::addOutput() {
    std::filesystem::path& output = output_.emplace();
    switch (kind_) {
    case Kind::files:
    case Kind::read_only:
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
    case Kind::terminal:
        output_option_ = app_.add_option("output", output,
                                         "The .gltf or .glb file to write. Its directory must exist; no file is overwritten")
                             ->required()
                             ->check([](const std::string& value) {
                                 return hasExtension(value, ".gltf") || hasExtension(value, ".glb")
                                            ? std::string()
                                            : std::string("OUTPUT must end in .gltf or .glb");
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
            throw common::systemFailure(ENOENT, common::Side::input, "file-unreadable",
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

nlohmann::ordered_json Command::jsonHeader(int format) const {
    return {{"format", format}, {"tool", {{"name", program()}, {"version", version()}}}};
}

void Command::reportFailure(const common::FailureReport& failure, bool json_requested) {
    std::cerr << program() << ": error: " << failure.message << '\n';
    diagnostics_.print(std::cerr, program());
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
    if (argc == 2 && std::strcmp(argv[1], "--summary") == 0) {
        std::cout << summary_ << '\n';
        return 0;
    }
    // --json must hold even when the command line itself is wrong.
    bool json_requested = false;
    for (int i = 1; i < argc; ++i) {
        json_requested = json_requested || std::strcmp(argv[i], "--json") == 0;
    }
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
        std::cerr << "Run '" << app_.get_name() << " --help' for usage.\n";
        return common::toInt(common::ExitCode::usage);
    }
    argv_.assign(argv, argv + argc);
    try {
        resolve();
        const int code = body();
        diagnostics_.print(std::cerr, program());
        return code;
    } catch (...) {
        const common::FailureReport failure = common::describeFailure(std::current_exception());
        reportFailure(failure, json_);
        if (failure.exit_code == common::ExitCode::usage) {
            std::cerr << "Run '" << app_.get_name() << " --help' for usage.\n";
        }
        return common::toInt(failure.exit_code);
    }
}

} // namespace meshtinker::cli
