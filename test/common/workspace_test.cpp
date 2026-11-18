#include "meshtinker/common/run.hpp"
#include "meshtinker/common/failure.hpp"
#include "meshtinker/common/workspace.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <filesystem>
#include <new>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace meshtinker::common;

TEST_CASE(is_created_on_first_use_and_removed) {
    const fs::path parent = meshtinker::test::workDirectory("workspace/removed");
    std::optional<fs::path> created;
    {
        Workspace workspace(parent, false);
        CHECK(!workspace.path().has_value());
        CHECK(fs::is_empty(parent));
        ScratchMapping map = workspace.scratch("coverage", 1 << 20);
        CHECK_EQ(map.size(), std::size_t{1} << 20);
        CHECK(map.data()[0] == 0 && map.data()[map.size() - 1] == 0);
        map.data()[12345] = 7;
        CHECK(map.data()[12345] == 7);
        created = workspace.path();
        CHECK(created.has_value() && fs::is_directory(*created));
        // The scratch file is already unlinked.
        CHECK(created.has_value() && fs::is_empty(*created));
        CHECK(created.has_value() && created->filename().string().rfind("meshtinker-", 0) == 0);
    }
    CHECK(fs::is_empty(parent));
}

TEST_CASE(kept_workspace_keeps_its_files) {
    const fs::path parent = meshtinker::test::workDirectory("workspace/kept");
    std::optional<fs::path> created;
    {
        Workspace workspace(parent, true);
        ScratchMapping map = workspace.scratch("coverage", 4096);
        map.data()[0] = 1;
        created = workspace.path();
    }
    CHECK(created.has_value() && fs::file_size(*created / "coverage") == 4096);
}

TEST_CASE(unwritable_parent_is_a_machine_problem) {
    Workspace workspace("/proc/no-such-dir", false);
    try {
        workspace.scratch("x", 16);
        CHECK(false);
    } catch (const Failure& failure) {
        CHECK(failure.exitCode() == ExitCode::machine);
        CHECK_EQ(failure.code(), std::string("workspace-failed"));
    }
}

TEST_CASE(describes_failures_by_class) {
    const auto describe = [](const auto& exception) { return describeFailure(std::make_exception_ptr(exception)); };
    CHECK(describe(rejected("invalid-gltf", "bad")).exit_code == ExitCode::input_rejected);
    CHECK_EQ(describe(rejected("invalid-gltf", "bad")).code, std::string("invalid-gltf"));
    CHECK(describe(std::bad_alloc()).exit_code == ExitCode::resource_limit);
    CHECK(describe(std::logic_error("oops")).exit_code == ExitCode::internal);
    const auto disk = describe(fs::filesystem_error("write", "/secret/folder/out.bin",
                                                    std::make_error_code(std::errc::no_space_on_device)));
    CHECK(disk.exit_code == ExitCode::resource_limit);
    CHECK(disk.message.find("/secret") == std::string::npos);
    CHECK(disk.message.find("out.bin") != std::string::npos);
}
