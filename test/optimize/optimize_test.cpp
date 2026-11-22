// optimize as a whole: the real subcommands run as children on fixtures, and
// an interruption that reaches the running child.

#include "meshtinker/gltf/document.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace meshtinker;
using Json = nlohmann::ordered_json;

namespace {

std::string quote(const std::string& text) { return "'" + text + "'"; }

struct Run {
    int code = -1;
    Json json;
};

Run optimize(const std::vector<std::string>& arguments, const fs::path& work) {
    std::string line = quote((fs::path(MESHTINKER_BUILD_DIR) / "meshtinker-optimize").string()) + " --json";
    for (const std::string& argument : arguments) {
        line += " " + quote(argument);
    }
    const fs::path out = work / "stdout.json";
    line += " > " + quote(out.string()) + " 2>/dev/null";
    const int status = std::system(line.c_str());
    Run result;
    result.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    std::ifstream stream(out);
    result.json = Json::parse(stream, nullptr, false);
    return result;
}

std::vector<std::string> commands(const Json& report) {
    std::vector<std::string> names;
    for (const Json& step : report.value("steps", Json::array())) {
        names.push_back(step.value("command", std::string()));
    }
    return names;
}

// Nothing but `keep` is left in `directory`: no workspace, no chain.
bool onlyHolds(const fs::path& directory, const std::vector<std::string>& keep) {
    for (const fs::directory_entry& entry : fs::directory_iterator(directory)) {
        const std::string name = entry.path().filename().string();
        if (std::find(keep.begin(), keep.end(), name) == keep.end()) {
            return false;
        }
    }
    return true;
}

} // namespace

// A textured scan goes through every step, the GLB reads back, and the
// workspace is gone.
TEST_CASE(scan_through_every_step) {
    const fs::path work = test::workDirectory("optimize/scan");
    const fs::path output = work / "scan.glb";
    const Run run = optimize({test::fixture("scan").model.string(), output.string()}, work);
    CHECK_EQ(run.code, 0);
    const std::vector<std::string> expected = {"import", "normals", "weld", "dedup", "simplify",
                                               "uv",     "ktx",     "compress", "export"};
    CHECK(commands(run.json) == expected);
    for (const Json& step : run.json["steps"]) {
        CHECK_EQ(step["exitCode"].get<int>(), 0);
    }
    CHECK(run.json["output"]["byteSize"].get<std::uint64_t>() == fs::file_size(output));
    CHECK(gltf::Document::read(output).data().meshes_count > 0);
    CHECK(onlyHolds(work, {"scan.glb", "stdout.json"}));
}

// Without textures: no uv, no image step. Thumb and fast change the options.
TEST_CASE(options_reach_the_steps) {
    const fs::path work = test::workDirectory("optimize/options");
    const Run run = optimize({"--quality", "Thumb", test::fixture("bare").model.string(), (work / "bare.glb").string()}, work);
    CHECK_EQ(run.code, 0);
    CHECK(commands(run.json) ==
          (std::vector<std::string>{"import", "normals", "weld", "dedup", "simplify", "compress", "export"}));
    CHECK_EQ(run.json["steps"][4]["argv"][2].get<std::string>(), std::string("0.01"));
    CHECK_EQ(run.json["settings"]["quality"].get<std::string>(), std::string("Thumb"));
}

// A step that fails makes optimize fail with its exit code and error, and the
// workspace goes.
TEST_CASE(failing_step_fails_optimize) {
    const fs::path work = test::workDirectory("optimize/fail");
    // A JSON file that isn't glTF: import rejects it.
    const fs::path not_gltf = test::fixture("triangle").model.parent_path() / "expected.json";
    const Run run = optimize({not_gltf.string(), (work / "out.glb").string()}, work);
    CHECK_EQ(run.code, 1);
    CHECK(run.json.contains("error"));
    CHECK(onlyHolds(work, {"stdout.json"}));
}

// SIGTERM reaches the running step, then optimize dies of it, leaving nothing
// behind. The step is a stand-in for weld that waits, in a copy of the build's
// commands.
TEST_CASE(interruption_reaches_the_step) {
    const fs::path work = test::workDirectory("optimize/interrupt");
    const fs::path bin = work / "bin";
    fs::create_directories(bin);
    for (const char* name : {"optimize", "import", "normals", "dedup", "export"}) {
        fs::copy_file(fs::path(MESHTINKER_BUILD_DIR) / (std::string("meshtinker-") + name),
                      bin / (std::string("meshtinker-") + name));
    }
    const fs::path started = work / "weld-started";
    const fs::path marker = work / "weld-got-term";
    {
        std::ofstream weld(bin / "meshtinker-weld");
        // Its sleep must not hold optimize's pipe: optimize reads the step's
        // stdout to the end.
        weld << "#!/bin/sh\ntrap 'touch " << marker.string() << "; kill $!; exit 143' TERM\ntouch " << started.string()
             << "\nsleep 30 > /dev/null &\nwait\n";
    }
    fs::permissions(bin / "meshtinker-weld", fs::perms::owner_all);
    const fs::path out = work / "out";
    fs::create_directories(out);
    const std::string program = (bin / "meshtinker-optimize").string();
    const std::string input = test::fixture("scan").model.string();
    const std::string output = (out / "scan.glb").string();
    std::vector<char*> argv = {const_cast<char*>(program.c_str()), const_cast<char*>(input.c_str()),
                               const_cast<char*>(output.c_str()), nullptr};
    pid_t pid = 0;
    CHECK_EQ(posix_spawn(&pid, program.c_str(), nullptr, nullptr, argv.data(), environ), 0);
    // Wait until the stand-in runs, and has set its trap.
    for (int i = 0; i < 400 && !fs::exists(started); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(fs::exists(started));
    ::kill(pid, SIGTERM);
    int status = 0;
    ::waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    CHECK(fs::exists(marker));
    CHECK(fs::is_empty(out));
}
