#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace meshtinker::common {

// Finding the `meshtinker-<name>` executables, as the `meshtinker` wrapper and
// meta-commands such as `optimize` do (AGENTS.md, Architecture): in
// `own_directory` first, the directory of the running executable, then on
// PATH.

// Subcommand names are lowercase words joined by dashes. Anything else could
// name a path ("../x") or an option, so it is never looked up.
bool isValidCommandName(const std::string& name);

// The executable that runs subcommand `name`, if any.
std::optional<std::filesystem::path> findCommand(const std::filesystem::path& own_directory, const std::string& name);

// Every subcommand found, by name, with the path that would run.
std::map<std::string, std::filesystem::path> findAllCommands(const std::filesystem::path& own_directory);

} // namespace meshtinker::common
