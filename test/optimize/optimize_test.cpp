// optimize as a whole: the real subcommands run as children on fixtures, and
// an interruption that reaches the running child.

#include "meshtinker/gltf/document.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>
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
    std::string err;
};

Run optimize(const std::vector<std::string>& arguments, const fs::path& work) {
    std::string line = quote((fs::path(MESHTINKER_BUILD_DIR) / "meshtinker-optimize").string()) + " --json";
    for (const std::string& argument : arguments) {
        line += " " + quote(argument);
    }
    const fs::path out = work / "stdout.json";
    // Beside the work directory, which tests expect to hold only outputs.
    const fs::path err = work.parent_path() / (work.filename().string() + "-stderr.log");
    line += " > " + quote(out.string()) + " 2> " + quote(err.string());
    const int status = std::system(line.c_str());
    Run result;
    result.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    std::ifstream stream(out);
    result.json = Json::parse(stream, nullptr, false);
    std::ifstream err_stream(err);
    result.err.assign(std::istreambuf_iterator<char>(err_stream), std::istreambuf_iterator<char>());
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
    const Run run = optimize({test::fixture("scan-atlas").model.string(), output.string()}, work);
    CHECK_EQ(run.code, 0);
    const std::vector<std::string> expected = {"import", "normals", "weld", "dedup", "simplify",
                                               "uv",     "ktx",     "compress", "export"};
    CHECK(commands(run.json) == expected);
    for (const Json& step : run.json["steps"]) {
        CHECK_EQ(step["exitCode"].get<int>(), 0);
    }
    CHECK(run.json["output"]["byteSize"].get<std::uint64_t>() == fs::file_size(output));
    CHECK(run.json["output"]["gpuBytes"].get<std::uint64_t>() < run.json["input"]["gpuBytes"].get<std::uint64_t>());
    CHECK(gltf::Document::read(output).data().meshes_count > 0);
    CHECK(onlyHolds(work, {"scan.glb", "stdout.json"}));
}

// --log-format json reaches every step: stderr is JSON lines only, each step
// logged as it starts and ends, and the options at debug.
TEST_CASE(json_logs_reach_every_step) {
    const fs::path work = test::workDirectory("optimize/logs");
    const Run run = optimize({"--log-format", "json", "--log-level", "debug", "--quality", "Thumb",
                              test::fixture("bare").model.string(), (work / "bare.glb").string()},
                             work);
    CHECK_EQ(run.code, 0);
    std::istringstream lines(run.err);
    std::vector<std::string> messages;
    for (std::string line; std::getline(lines, line);) {
        const Json object = Json::parse(line, nullptr, false);
        CHECK(object.is_object());
        if (object.is_object()) {
            CHECK(object.contains("level"));
            messages.push_back(object.value("message", std::string()));
        }
    }
    CHECK(std::find(messages.begin(), messages.end(), "Running import") != messages.end());
    CHECK(std::find(messages.begin(), messages.end(), "Running export") != messages.end());
    CHECK(std::find_if(messages.begin(), messages.end(), [](const std::string& m) {
              return m.rfind("export took ", 0) == 0;
          }) != messages.end());
    CHECK(std::find_if(messages.begin(), messages.end(), [](const std::string& m) {
              return m.rfind("meshtinker-import --json --log-level debug --log-format json", 0) == 0;
          }) != messages.end());
    for (const Json& step : run.json["steps"]) {
        CHECK_EQ(step["argv"][1].get<std::string>(), std::string("--log-level"));
    }
}

namespace {

// A copy of the build's commands in `bin`, with `meshtinker-<name>` replaced
// by a shell script.
fs::path commandsWithStandIn(const fs::path& work, const std::string& name, const std::string& script) {
    const fs::path bin = work / "bin";
    fs::create_directories(bin);
    for (const char* command : {"optimize", "import", "normals", "weld", "dedup", "simplify", "uv", "ktx", "compress",
                                "export"}) {
        if (command != name) {
            fs::copy_file(fs::path(MESHTINKER_BUILD_DIR) / (std::string("meshtinker-") + command),
                          bin / (std::string("meshtinker-") + command));
        }
    }
    {
        std::ofstream stand_in(bin / ("meshtinker-" + name));
        stand_in << "#!/bin/sh\n" << script;
    }
    fs::permissions(bin / ("meshtinker-" + name), fs::perms::owner_all);
    return bin;
}

} // namespace

// A step that fails, or dies, is left out with a warning: the next one goes
// on from the stage before it, and optimize succeeds.
TEST_CASE(failing_step_is_left_out) {
    for (const char* script : {"echo '{\"error\": {\"code\": \"broken\", \"message\": \"broken on purpose\"}}'\nexit 1\n",
                               "kill -KILL $$\n"}) {
        const fs::path work = test::workDirectory("optimize/left-out");
        const fs::path bin = commandsWithStandIn(work, "simplify", script);
        const fs::path out = work / "out";
        fs::create_directories(out);
        const std::string line = quote((bin / "meshtinker-optimize").string()) + " --json " +
                                 quote(test::fixture("scan-atlas").model.string()) + " " +
                                 quote((out / "scan.glb").string()) + " > " + quote((work / "stdout.json").string()) +
                                 " 2>/dev/null";
        const int status = std::system(line.c_str());
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        std::ifstream stream(work / "stdout.json");
        const Json json = Json::parse(stream, nullptr, false);
        CHECK(commands(json) ==
              (std::vector<std::string>{"import", "normals", "weld", "dedup", "simplify", "uv", "ktx", "compress", "export"}));
        CHECK(json["steps"][4]["exitCode"].get<int>() != 0 || !json["steps"][4]["signal"].is_null());
        bool warned = false;
        for (const Json& diagnostic : json["diagnostics"]) {
            warned = warned || diagnostic["code"].get<std::string>() == "step-failed";
        }
        CHECK(warned);
        CHECK(gltf::Document::read(out / "scan.glb").data().meshes_count > 0);
    }
}

// uv lists the images of groups it leaves alone, without an output: they
// keep their size, so the texture step still encodes them.
TEST_CASE(images_uv_leaves_keep_their_size) {
    const fs::path work = test::workDirectory("optimize/uv-no-gain");
    const fs::path bin = commandsWithStandIn(
        work, "uv",
        "echo '{\"groups\": [{\"status\": \"no-gain\", \"images\": [{\"source\": \"scan-atlas_texture_0.jpg\", "
        "\"width\": 1024, \"height\": 1024}]}], \"diagnostics\": []}'\n");
    const fs::path out = work / "out";
    fs::create_directories(out);
    const std::string line = quote((bin / "meshtinker-optimize").string()) + " --json " +
                             quote(test::fixture("scan-atlas").model.string()) + " " +
                             quote((out / "scan.glb").string()) + " > " + quote((work / "stdout.json").string()) +
                             " 2>/dev/null";
    const int status = std::system(line.c_str());
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    std::ifstream stream(work / "stdout.json");
    const Json json = Json::parse(stream, nullptr, false);
    CHECK(commands(json) ==
          (std::vector<std::string>{"import", "normals", "weld", "dedup", "simplify", "uv", "ktx", "compress", "export"}));
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

// An import that fails makes optimize fail with its exit code and error, and
// the workspace goes.
TEST_CASE(failing_import_fails_optimize) {
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
    const fs::path started = work / "weld-started";
    const fs::path marker = work / "weld-got-term";
    // Its sleep must not hold optimize's pipe: optimize reads the step's
    // stdout to the end.
    const fs::path bin = commandsWithStandIn(work, "weld",
                                             "trap 'touch " + marker.string() + "; kill $!; exit 143' TERM\ntouch " +
                                                 started.string() + "\nsleep 30 > /dev/null &\nwait\n");
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
