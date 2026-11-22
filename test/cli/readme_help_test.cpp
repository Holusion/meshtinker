// The README's option blocks are copies of `--help` output (AGENTS.md,
// "Command-line conventions"). Each is a fenced block whose first line is
// `$ meshtinker <command> --help` or `$ meshtinker --help`; the rest of the
// block must be exactly what that command prints. Every built command needs one.

#include "meshtinker/common/process.hpp"
#include "support/check.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace {

std::string trimTrailingNewlines(std::string text) {
    while (!text.empty() && text.back() == '\n') {
        text.pop_back();
    }
    return text;
}

// README blocks by command ("" for the wrapper).
std::map<std::string, std::string> readmeBlocks() {
    std::ifstream stream(fs::path(MESHTINKER_TEST_DATA).parent_path().parent_path() / "README.md");
    std::map<std::string, std::string> blocks;
    std::string line;
    bool in_block = false;
    bool first = false;
    std::optional<std::string> command;
    std::string body;
    const std::regex help_line(R"(^\$ meshtinker(?: ([a-z0-9-]+))? --help$)");
    while (std::getline(stream, line)) {
        if (line.rfind("```", 0) == 0) {
            if (in_block && command) {
                blocks[*command] = trimTrailingNewlines(body);
            }
            in_block = !in_block;
            first = true;
            command.reset();
            body.clear();
            continue;
        }
        if (!in_block) {
            continue;
        }
        std::smatch match;
        if (first && std::regex_match(line, match, help_line)) {
            command = match[1].str();
        } else if (command) {
            body += line + "\n";
        }
        first = false;
    }
    return blocks;
}

} // namespace

TEST_CASE(readme_blocks_match_help_output) {
    // An empty PATH, so the wrapper lists this build's commands only.
    ::setenv("PATH", "", 1);
    const fs::path build(MESHTINKER_BUILD_DIR);
    const std::map<std::string, std::string> blocks = readmeBlocks();
    std::vector<std::string> commands{""};
    for (const auto& entry : fs::directory_iterator(build)) {
        const std::string file = entry.path().filename().string();
        if (file.rfind("meshtinker-", 0) == 0 && meshtinker::common::isExecutableFile(entry.path())) {
            commands.push_back(file.substr(11));
        }
    }
    for (const std::string& command : commands) {
        const fs::path program = build / (command.empty() ? "meshtinker" : "meshtinker-" + command);
        const auto output = meshtinker::common::captureOutput(program, {program.filename().string(), "--help"});
        CHECK(output.has_value());
        const auto block = blocks.find(command);
        if (block == blocks.end()) {
            meshtinker::test::fail("README has no `$ meshtinker " + command + (command.empty() ? "" : " ") +
                                       "--help` block",
                                   __FILE__, __LINE__);
            continue;
        }
        // The wrapper lists the commands of this build: the README's list
        // loses those that weren't built, such as `avif` with MESHTINKER_AVIF off.
        std::string expected = block->second;
        if (command.empty()) {
            static const std::regex listed(R"(^  ([a-z]+) {2,}\S.*$)");
            std::istringstream lines(block->second);
            std::string line;
            std::string kept;
            while (std::getline(lines, line)) {
                std::smatch match;
                if (std::regex_match(line, match, listed) &&
                    std::find(commands.begin(), commands.end(), match[1].str()) == commands.end()) {
                    continue;
                }
                kept += line + "\n";
            }
            expected = trimTrailingNewlines(kept);
        }
        if (output && trimTrailingNewlines(*output) != expected) {
            meshtinker::test::fail("README block for `meshtinker " + command + " --help` is out of date. Replace it with:\n" +
                                       trimTrailingNewlines(*output),
                                   __FILE__, __LINE__);
        }
    }
}
