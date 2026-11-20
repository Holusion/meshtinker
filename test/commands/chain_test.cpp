// Chain mode end to end: the commands as built, run as a user runs them
// (docs/chaining.md).

#include "meshtinker/chain/chain.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <cstdlib>
#include <fstream>
#include <sys/wait.h>

namespace fs = std::filesystem;
using namespace meshtinker;
using Json = nlohmann::ordered_json;

namespace {

struct Run {
    int code = -1;
    Json json;
};

std::string quote(const std::string& text) { return "'" + text + "'"; }

// Runs `meshtinker-<command>` with `arguments` and --json, and parses stdout.
Run run(const std::string& command, const std::vector<std::string>& arguments) {
    static int count = 0;
    const fs::path out = test::workDirectory("chain-commands/out-" + std::to_string(count++)) / "stdout.json";
    std::string line = quote((fs::path(MESHTINKER_BUILD_DIR) / ("meshtinker-" + command)).string()) + " --json";
    for (const std::string& argument : arguments) {
        line += " " + quote(argument);
    }
    line += " > " + quote(out.string()) + " 2>/dev/null";
    const int status = std::system(line.c_str());
    Run result;
    result.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    std::ifstream stream(out);
    result.json = Json::parse(stream, nullptr, false);
    return result;
}

bool hasDiagnostic(const Json& report, const std::string& code) {
    for (const Json& diagnostic : report.value("diagnostics", Json::array())) {
        if (diagnostic.value("code", std::string()) == code) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE(import_simplify_compress_inspect) {
    const fs::path root = test::workDirectory("chain-commands/scan");
    const std::string work = (root / "work").string();
    const fs::path source = test::fixture("scan").model;

    const Run imported = run("import", {source.string(), work});
    CHECK_EQ(imported.code, 0);
    const chain::Chain chain = chain::Chain::open(work);
    CHECK_EQ(chain.source(), source.filename().string());
    const chain::StageName first = chain.last();
    CHECK_EQ(first.command, std::string("import"));
    // Stage 000's report is import's, which is the inspect report.
    const Json first_record = chain.record(first);
    CHECK_EQ(first_record["report"]["geometry"], imported.json["geometry"]);
    CHECK_EQ(first_record["lossy"]["imageEncoding"][0]["format"].get<std::string>(), std::string("image/jpeg"));

    // inspect reads the last stage.
    const Run inspected = run("inspect", {work});
    CHECK_EQ(inspected.code, 0);
    CHECK_EQ(inspected.json["geometry"], imported.json["geometry"]);

    const Run simplified = run("simplify", {"--error", "0.01", work});
    CHECK_EQ(simplified.code, 0);
    const chain::StageName second = chain.last();
    CHECK_EQ(second.number, std::uint64_t{1});
    const Json second_record = chain.record(second);
    CHECK_EQ(second_record["parent"].get<std::string>(), first.text());
    CHECK_EQ(second_record["command"].get<std::string>(), std::string("simplify"));
    CHECK_EQ(second_record["report"], simplified.json);
    CHECK(second_record["lossy"]["simplificationError"].get<double>() > 0);
    CHECK(second_record["lossy"]["simplificationError"].get<double>() <= 0.01);
    CHECK_EQ(second_record["argv"].back().get<std::string>(), work);
    const Run after_simplify = run("inspect", {work});
    CHECK(after_simplify.json["geometry"]["faces"].get<std::uint64_t>() <
          imported.json["geometry"]["faces"].get<std::uint64_t>());

    const Run compressed = run("compress", {work});
    CHECK_EQ(compressed.code, 0);
    CHECK(!hasDiagnostic(compressed.json, "repeated-quantization"));
    const Json third = chain.record(chain.last())["lossy"];
    CHECK_EQ(third["geometryQuantization"][0]["positionBits"].get<int>(), 14);
    CHECK_EQ(third["simplificationError"], second_record["lossy"]["simplificationError"]);
    CHECK_EQ(chain.stages().size(), std::size_t{3});

    // A chain takes no output, and a plain file needs one.
    CHECK_EQ(run("simplify", {work, (root / "out.gltf").string()}).code, 2);
    CHECK_EQ(run("simplify", {source.string()}).code, 2);
    // A second chain can't go where the first one is.
    const Run again = run("import", {source.string(), work});
    CHECK_EQ(again.code, 4);
    CHECK_EQ(again.json["error"]["code"].get<std::string>(), std::string("output-exists"));
    CHECK_EQ(chain.stages().size(), std::size_t{3});
}

TEST_CASE(quantizing_again_warns) {
    const fs::path root = test::workDirectory("chain-commands/draco");
    const std::string work = (root / "work").string();
    CHECK_EQ(run("import", {test::fixture("draco").model.string(), work}).code, 0);
    const chain::Chain chain = chain::Chain::open(work);
    const Json first = chain.record(chain.last())["lossy"];
    CHECK_EQ(first["geometryQuantization"][0]["source"].get<std::string>(), std::string("KHR_draco_mesh_compression"));
    // simplify decodes Draco and writes floats; compress quantizes them again.
    CHECK_EQ(run("simplify", {work}).code, 0);
    const Run compressed = run("compress", {work});
    CHECK_EQ(compressed.code, 0);
    CHECK(hasDiagnostic(compressed.json, "repeated-quantization"));
    CHECK(hasDiagnostic(chain.record(chain.last())["report"], "repeated-quantization"));
    CHECK_EQ(chain.record(chain.last())["lossy"]["geometryQuantization"].size(), std::size_t{2});
}

TEST_CASE(busy_and_broken_chains) {
    const fs::path root = test::workDirectory("chain-commands/busy");
    const std::string work = (root / "work").string();
    CHECK_EQ(run("import", {test::fixture("cube").model.string(), work}).code, 0);
    {
        const std::optional<common::FileLock> held = common::FileLock::tryAcquire(fs::path(work) / "lock");
        const Run busy = run("compress", {work});
        CHECK_EQ(busy.code, 4);
        CHECK_EQ(busy.json["error"]["code"].get<std::string>(), std::string("chain-in-use"));
        // Readers take no lock.
        CHECK_EQ(run("inspect", {work}).code, 0);
    }
    CHECK_EQ(chain::Chain::open(work).stages().size(), std::size_t{1});
    // A failed transform leaves no stage, no temporary directory and no workspace.
    fs::create_directory(root / "plain");
    CHECK_EQ(run("compress", {(root / "plain").string()}).code, 1);
    std::size_t entries = 0;
    for ([[maybe_unused]] const auto& entry : fs::directory_iterator(work)) {
        ++entries;
    }
    CHECK_EQ(entries, std::size_t{3});
}
