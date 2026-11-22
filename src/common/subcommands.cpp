#include "meshtinker/common/subcommands.hpp"

#include "meshtinker/common/process.hpp"

#include <algorithm>
#include <string_view>
#include <system_error>
#include <vector>

namespace meshtinker::common {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view command_prefix = "meshtinker-";

// The directories searched for subcommands, in order.
std::vector<fs::path> searchDirectories(const fs::path& own_directory) {
    std::vector<fs::path> directories;
    if (!own_directory.empty()) {
        directories.push_back(own_directory);
    }
    for (const fs::path& directory : searchPath()) {
        directories.push_back(directory);
    }
    return directories;
}

} // namespace

bool isValidCommandName(const std::string& name) {
    if (name.empty() || name.front() == '-' || name.back() == '-') {
        return false;
    }
    return std::all_of(name.begin(), name.end(),
                       [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

std::optional<fs::path> findCommand(const fs::path& own_directory, const std::string& name) {
    if (!isValidCommandName(name)) {
        return std::nullopt;
    }
    for (const fs::path& directory : searchDirectories(own_directory)) {
        fs::path candidate = directory / (std::string(command_prefix) + name);
        if (isExecutableFile(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::map<std::string, fs::path> findAllCommands(const fs::path& own_directory) {
    std::map<std::string, fs::path> commands;
    for (const fs::path& directory : searchDirectories(own_directory)) {
        std::error_code error;
        for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
            const std::string file = it->path().filename().string();
            if (file.rfind(command_prefix, 0) != 0) {
                continue;
            }
            const std::string name = file.substr(command_prefix.size());
            if (isValidCommandName(name) && commands.count(name) == 0 && isExecutableFile(it->path())) {
                commands.emplace(name, it->path());
            }
        }
    }
    return commands;
}

} // namespace meshtinker::common
