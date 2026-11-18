#include "meshtinker/common/process.hpp"
#include "support/check.hpp"

#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;
using namespace meshtinker::common;

TEST_CASE(current_executable_is_this_test) {
    const fs::path path = currentExecutable(nullptr);
    CHECK(path.is_absolute());
    CHECK_EQ(path.filename().string(), std::string("test-common-process"));
    CHECK(isExecutableFile(path));
}

TEST_CASE(directories_are_not_executable) {
    CHECK(!isExecutableFile(fs::current_path()));
    CHECK(!isExecutableFile(fs::path(MESHTINKER_TEST_DATA) / "does-not-exist"));
}

TEST_CASE(search_path_skips_empty_entries) {
    ::setenv("PATH", ":/a::/b:", 1);
    const auto directories = searchPath();
    CHECK_EQ(directories.size(), 2u);
    CHECK(directories.size() == 2 && directories[0] == fs::path("/a") && directories[1] == fs::path("/b"));
}
