// The `meshtinker` wrapper: `meshtinker <name> [args...]` runs `meshtinker-<name>`,
// found in the wrapper's own directory first, then on PATH, in the style of git.

#include "meshtinker/cli/command.hpp"
#include "meshtinker/common/exit_code.hpp"
#include "meshtinker/common/process.hpp"
#include "meshtinker/common/subcommands.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace meshtinker::wrapper {
namespace {

namespace fs = std::filesystem;
using common::ExitCode;
using common::toInt;

constexpr const char* command_prefix = "meshtinker-";

// Each subcommand describes itself in one line through the hidden --summary flag.
std::string summaryOf(const fs::path& program) {
    const std::optional<std::string> output = common::captureOutput(program, {program.filename().string(), "--summary"});
    if (!output || output->empty()) {
        return "(no summary)";
    }
    return output->substr(0, output->find('\n'));
}

void printUsage(std::ostream& out, const fs::path& own_directory) {
    out << "Analyse and optimize glTF 2.0 assets, using little memory.\n"
           "\n"
           "Usage: meshtinker <command> [args...]\n"
           "       meshtinker help <command>\n"
           "       meshtinker --version\n"
           "\n"
           "Commands:\n";
    const std::map<std::string, fs::path> commands = common::findAllCommands(own_directory);
    std::size_t width = 0;
    for (const auto& command : commands) {
        width = std::max(width, command.first.size());
    }
    for (const auto& [name, program] : commands) {
        out << "  " << name << std::string(width - name.size() + 3, ' ') << summaryOf(program) << '\n';
    }
    if (commands.empty()) {
        out << "  (none found next to meshtinker or on PATH)\n";
    }
    out << "\n"
           "Commands are the meshtinker-<command> executables next to meshtinker, then on PATH.\n";
}

int runCommand(const fs::path& own_directory, const std::string& name, std::vector<std::string> arguments) {
    if (!common::isValidCommandName(name)) {
        std::cerr << "meshtinker: '" << name << "' is not a command name\n";
        return toInt(ExitCode::usage);
    }
    const std::optional<fs::path> program = common::findCommand(own_directory, name);
    if (!program) {
        std::cerr << "meshtinker: '" << name << "' is not a meshtinker command. See 'meshtinker --help'.\n";
        return toInt(ExitCode::usage);
    }
    arguments.insert(arguments.begin(), command_prefix + name);
    const std::string error = common::replaceProcess(*program, arguments);
    std::cerr << "meshtinker: cannot run " << program->filename().string() << ": " << error << '\n';
    return toInt(ExitCode::machine);
}

int run(int argc, char** argv) {
    const fs::path own_directory = common::currentExecutable(argv[0]).parent_path();
    if (argc < 2) {
        printUsage(std::cerr, own_directory);
        return toInt(ExitCode::usage);
    }
    const std::string first = argv[1];
    if (first == "--help" || first == "-h" || (first == "help" && argc == 2)) {
        printUsage(std::cout, own_directory);
        return toInt(ExitCode::success);
    }
    if (first == "--version") {
        std::cout << "meshtinker " << cli::version() << '\n';
        return toInt(ExitCode::success);
    }
    if (first == "help") {
        return runCommand(own_directory, argv[2], {"--help"});
    }
    return runCommand(own_directory, first, std::vector<std::string>(argv + 2, argv + argc));
}

} // namespace
} // namespace meshtinker::wrapper

int main(int argc, char** argv) { return meshtinker::wrapper::run(argc, argv); }
