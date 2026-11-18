#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace meshtinker::common {

// Absolute path of the running executable with symlinks resolved, so a
// symlinked `meshtinker` finds the subcommands installed next to its target.
// Uses the operating system's record of the executable and falls back to
// resolving `argv0` against the working directory and PATH. Empty if neither works.
std::filesystem::path currentExecutable(const char* argv0);

// True if `path` is a regular file the current user may execute.
bool isExecutableFile(const std::filesystem::path& path);

// Directories listed in PATH, in order. Empty entries are skipped: they would
// mean the working directory, which is not a place to look for commands.
std::vector<std::filesystem::path> searchPath();

// Replaces the current process with `program`, passing `arguments` as argv
// (arguments[0] included). Returns only on failure, with a description of the error.
std::string replaceProcess(const std::filesystem::path& program, const std::vector<std::string>& arguments);

// Runs `program` with `arguments` (argv[0] included), stdin from /dev/null,
// and returns what it printed on stdout. Empty if it can't be run or exits
// with a non-zero status. Its stderr is discarded.
std::optional<std::string> captureOutput(const std::filesystem::path& program, const std::vector<std::string>& arguments);

} // namespace meshtinker::common
