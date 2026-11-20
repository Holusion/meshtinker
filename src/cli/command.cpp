#include "meshtinker/cli/command.hpp"

#include "meshtinker/common/cleanup.hpp"
#include "meshtinker/common/failure.hpp"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace meshtinker::cli {

const char* version() { return MESHTINKER_VERSION; }

Command::Command(std::string name, std::string summary, const std::string& description)
    : name_(std::move(name)), summary_(std::move(summary)),
      app_(description.empty() ? summary_ : summary_ + "\n\n" + description, "meshtinker " + name_) {
    app_.set_version_flag("--version", std::string("meshtinker ") + version(), "Print the version and exit");
    // Help is the same whatever the terminal, so README blocks can be checked against it.
    app_.get_formatter()->column_width(32);
}

void Command::addInput() { app_.add_option("input", input_, "The .gltf or .glb file")->required(); }

void Command::addOutput() {
    std::filesystem::path& output = output_.emplace();
    app_.add_option("output", output, "The .gltf file to write. Its directory must exist; no file is overwritten")
        ->required()
        ->check([](const std::string& value) {
            return std::filesystem::u8path(value).extension() == ".gltf" ? std::string()
                                                                         : std::string("OUTPUT must end in .gltf");
        });
}

void Command::addJsonOption() {
    app_.add_flag("--json", json_, "Print the result as one JSON object on stdout, even on failure");
}

void Command::addTrustedOption() {
    app_.add_flag("--trusted", trusted_,
                  "Let the model reference files outside its folder: absolute paths, `..` leading out, symlinks");
}

void Command::addWorkspaceOptions() {
    const std::string default_location = output_ ? "the output's directory" : "the input's directory";
    app_.add_option("--tmp-dir", tmp_dir_, "Where to create the temporary workspace (default: " + default_location + ")")
        ->check(CLI::ExistingDirectory);
    app_.add_flag("--keep-tmp", keep_tmp_, "Keep the temporary workspace and print its path, for debugging");
}

std::filesystem::path Command::outputDirectory() const {
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
    if (!output_) {
        throw std::logic_error("outputStem() without addOutput()");
    }
    return output_->stem().u8string();
}

common::Workspace Command::workspace() const {
    if (tmp_dir_) {
        return common::Workspace(*tmp_dir_, keep_tmp_);
    }
    return common::Workspace(output_ ? outputDirectory() : input_.parent_path(), keep_tmp_);
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
    try {
        const int code = body();
        diagnostics_.print(std::cerr, program());
        return code;
    } catch (...) {
        const common::FailureReport failure = common::describeFailure(std::current_exception());
        reportFailure(failure, json_);
        return common::toInt(failure.exit_code);
    }
}

} // namespace meshtinker::cli
