#include "meshtinker/common/failure.hpp"
#include "meshtinker/common/outputs.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <csignal>
#include <fstream>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace meshtinker::common;

namespace {

std::size_t entries(const fs::path& directory) {
    std::size_t count = 0;
    for ([[maybe_unused]] const auto& entry : fs::directory_iterator(directory)) {
        ++count;
    }
    return count;
}

std::string contents(const fs::path& path) {
    std::ifstream stream(path);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void writeFile(Outputs& outputs, const fs::path& path, const std::string& text) {
    OutputFile file = outputs.create(path);
    file.write(text);
    file.close();
}

} // namespace

TEST_CASE(publishes_only_on_commit) {
    const fs::path directory = meshtinker::test::workDirectory("outputs/commit");
    {
        Outputs outputs;
        writeFile(outputs, directory / "a.gltf", "a");
        writeFile(outputs, directory / "a.bin", "bin");
        CHECK(!fs::exists(directory / "a.gltf"));
        CHECK_EQ(entries(directory), 2u); // the temporary files
        // Visible, named after their destination and marked as meshtinker's,
        // so a killed run's leftovers are easy to find and to clean up.
        for (const auto& entry : fs::directory_iterator(directory)) {
            const std::string name = entry.path().filename().string();
            CHECK(name.front() != '.');
            CHECK(name.rfind("a.gltf.meshtinker-", 0) == 0 || name.rfind("a.bin.meshtinker-", 0) == 0);
            CHECK(name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0);
        }
        outputs.commit();
    }
    CHECK_EQ(contents(directory / "a.gltf"), std::string("a"));
    CHECK_EQ(contents(directory / "a.bin"), std::string("bin"));
    CHECK_EQ(entries(directory), 2u);
}

TEST_CASE(leaves_nothing_without_commit) {
    const fs::path directory = meshtinker::test::workDirectory("outputs/abandon");
    {
        Outputs outputs;
        writeFile(outputs, directory / "a.gltf", "a");
    }
    CHECK_EQ(entries(directory), 0u);
}

TEST_CASE(never_replaces_a_file) {
    const fs::path directory = meshtinker::test::workDirectory("outputs/exists");
    std::ofstream(directory / "taken.gltf") << "mine";
    Outputs outputs;
    try {
        outputs.create(directory / "taken.gltf");
        CHECK(false);
    } catch (const Failure& failure) {
        CHECK_EQ(failure.code(), std::string("output-exists"));
        CHECK(failure.exitCode() == ExitCode::machine);
    }
    // A file that appears between create() and commit() is kept, and
    // everything this run wrote, published or not, is removed.
    {
        Outputs late;
        writeFile(late, directory / "first.bin", "1");
        writeFile(late, directory / "late.gltf", "ours");
        std::ofstream(directory / "late.gltf") << "theirs";
        try {
            late.commit();
            CHECK(false);
        } catch (const Failure& failure) {
            CHECK_EQ(failure.code(), std::string("output-exists"));
        }
    }
    CHECK_EQ(contents(directory / "late.gltf"), std::string("theirs"));
    CHECK(!fs::exists(directory / "first.bin"));
    CHECK_EQ(entries(directory), 2u);
}

TEST_CASE(missing_directory_is_a_machine_problem) {
    const fs::path directory = meshtinker::test::workDirectory("outputs/missing");
    Outputs outputs;
    try {
        outputs.create(directory / "no-such-folder" / "a.gltf");
        CHECK(false);
    } catch (const Failure& failure) {
        CHECK(failure.exitCode() == ExitCode::machine);
    }
}

TEST_CASE(interruption_removes_temporary_files) {
    const fs::path directory = meshtinker::test::workDirectory("outputs/interrupt");
    const pid_t child = ::fork();
    if (child == 0) {
        installInterruptCleanup();
        Outputs outputs;
        OutputFile file = outputs.create(directory / "a.gltf");
        file.write(std::string("partial"));
        std::raise(SIGTERM);
        ::_exit(0); // not reached
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    CHECK_EQ(entries(directory), 0u);
}
