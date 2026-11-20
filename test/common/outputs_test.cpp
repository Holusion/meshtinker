#include "meshtinker/common/failure.hpp"
#include "meshtinker/common/file_lock.hpp"
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

TEST_CASE(publishes_a_directory_whole) {
    const fs::path directory = meshtinker::test::workDirectory("outputs/directory");
    {
        Outputs outputs;
        const fs::path pending = outputs.createDirectory(directory / "000-import-abcde");
        CHECK(fs::is_directory(pending));
        const std::string name = pending.filename().string();
        CHECK(name.rfind("000-import-abcde.meshtinker-", 0) == 0);
        CHECK(name.compare(name.size() - 4, 4, ".tmp") == 0);
        // Files inside are written under their own names: the directory
        // publishes them.
        writeFile(outputs, pending / "a.gltf", "a");
        CHECK(fs::exists(pending / "a.gltf"));
        writeFile(outputs, directory / "chain.json", "{}");
        CHECK(!fs::exists(directory / "000-import-abcde"));
        outputs.commit();
    }
    CHECK_EQ(contents(directory / "000-import-abcde" / "a.gltf"), std::string("a"));
    CHECK_EQ(contents(directory / "chain.json"), std::string("{}"));
    CHECK_EQ(entries(directory), 2u);
    CHECK_EQ(entries(directory / "000-import-abcde"), 1u);
}

TEST_CASE(abandoned_directory_leaves_nothing) {
    const fs::path directory = meshtinker::test::workDirectory("outputs/directory-abandon");
    {
        Outputs outputs;
        const fs::path pending = outputs.createDirectory(directory / "stage");
        writeFile(outputs, pending / "a.gltf", "a");
    }
    CHECK_EQ(entries(directory), 0u);
}

TEST_CASE(never_replaces_a_directory) {
    const fs::path directory = meshtinker::test::workDirectory("outputs/directory-exists");
    fs::create_directory(directory / "taken");
    Outputs outputs;
    try {
        outputs.createDirectory(directory / "taken");
        CHECK(false);
    } catch (const Failure& failure) {
        CHECK_EQ(failure.code(), std::string("output-exists"));
    }
    // A directory that appears before commit() is kept, even empty.
    {
        Outputs late;
        const fs::path pending = late.createDirectory(directory / "late");
        writeFile(late, pending / "a.gltf", "a");
        fs::create_directory(directory / "late");
        try {
            late.commit();
            CHECK(false);
        } catch (const Failure& failure) {
            CHECK_EQ(failure.code(), std::string("output-exists"));
        }
    }
    CHECK(fs::is_empty(directory / "late"));
    CHECK_EQ(entries(directory), 2u);
}

TEST_CASE(interruption_removes_a_pending_directory) {
    const fs::path directory = meshtinker::test::workDirectory("outputs/directory-interrupt");
    const pid_t child = ::fork();
    if (child == 0) {
        installInterruptCleanup();
        Outputs outputs;
        // Registered in an order that nesting doesn't follow.
        OutputFile outside = outputs.create(directory / "chain.json");
        const fs::path pending = outputs.createDirectory(directory / "stage");
        OutputFile file = outputs.create(pending / "a.gltf");
        file.write(std::string("partial"));
        std::raise(SIGTERM);
        ::_exit(0); // not reached
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    CHECK_EQ(entries(directory), 0u);
}

TEST_CASE(copies_and_links_whole_files) {
    const fs::path directory = meshtinker::test::workDirectory("outputs/copy");
    fs::create_directory(directory / "parent");
    std::ofstream(directory / "parent" / "image.png") << "pixels";
    const std::string bytes = "pixels";
    const ByteView view{reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
    {
        Outputs outputs;
        const fs::path pending = outputs.createDirectory(directory / "copied");
        CHECK_EQ(outputs.copy(directory / "parent" / "image.png", view, pending / "image.png"), std::uint64_t{6});
        CHECK_EQ(outputs.linkedCount(), std::size_t{0});
        outputs.commit();
    }
    CHECK_EQ(contents(directory / "copied" / "image.png"), bytes);
    CHECK_EQ(fs::hard_link_count(directory / "parent" / "image.png"), std::uintmax_t{1});
    {
        Outputs outputs;
        outputs.linkFrom(directory / "parent");
        const fs::path pending = outputs.createDirectory(directory / "linked");
        outputs.copy(directory / "parent" / "image.png", view, pending / "image.png");
        CHECK_EQ(outputs.linkedCount(), std::size_t{1});
        outputs.commit();
    }
    CHECK(fs::equivalent(directory / "linked" / "image.png", directory / "parent" / "image.png"));
    // A file that changed size since it was mapped: the mapped bytes are written.
    {
        Outputs outputs;
        const std::string shorter = "pix";
        outputs.copy(directory / "parent" / "image.png",
                     ByteView{reinterpret_cast<const std::uint8_t*>(shorter.data()), shorter.size()},
                     directory / "mapped.png");
        outputs.commit();
    }
    CHECK_EQ(contents(directory / "mapped.png"), std::string("pix"));
    // A failed run removes its links, and the parent's file stays.
    {
        Outputs outputs;
        outputs.linkFrom(directory / "parent");
        outputs.copy(directory / "parent" / "image.png", view, directory / "abandoned.png");
    }
    CHECK(!fs::exists(directory / "abandoned.png"));
    CHECK_EQ(contents(directory / "parent" / "image.png"), bytes);
}

TEST_CASE(file_lock_is_exclusive_across_processes) {
    const fs::path directory = meshtinker::test::workDirectory("outputs/lock");
    std::optional<FileLock> lock = FileLock::tryAcquire(directory / "lock");
    CHECK(lock.has_value());
    CHECK(lock && lock->created());
    const pid_t child = ::fork();
    if (child == 0) {
        const std::optional<FileLock> other = FileLock::tryAcquire(directory / "lock");
        ::_exit(other ? 1 : 0);
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    lock.reset();
    const std::optional<FileLock> again = FileLock::tryAcquire(directory / "lock");
    CHECK(again.has_value());
    CHECK(again && !again->created());
}
