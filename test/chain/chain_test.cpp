#include "meshtinker/chain/chain.hpp"
#include "meshtinker/common/failure.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <fstream>

namespace fs = std::filesystem;
using namespace meshtinker;
using chain::Json;

namespace {

void writeText(const fs::path& path, const std::string& text) { std::ofstream(path, std::ios::binary) << text; }

std::string failureCode(const std::function<void()>& action) {
    try {
        action();
    } catch (const common::Failure& failure) {
        return failure.code();
    }
    return "";
}

// Runs a stage the way a command does: writes its model, finishes, commits.
void writeStage(chain::StageWriter& stage, common::Outputs& outputs, const std::string& model) {
    common::OutputFile file = outputs.create(stage.directory() / (stage.stem() + ".gltf"));
    file.write(model);
    file.close();
    stage.finish(Json{{"answer", 42}}, "meshtinker-test 0.0.0", {"meshtinker-test", "work"});
    outputs.commit();
    stage.published();
}

} // namespace

TEST_CASE(parses_stage_names) {
    const auto simple = chain::parseStageName("001-simplify-7xd2m");
    CHECK(simple.has_value());
    CHECK_EQ(simple->number, std::uint64_t{1});
    CHECK_EQ(simple->command, std::string("simplify"));
    CHECK_EQ(simple->id, std::string("7xd2m"));
    CHECK_EQ(simple->text(), std::string("001-simplify-7xd2m"));
    // A command name with hyphens: the number is before the first one, the
    // id after the last one.
    const auto variant = chain::parseStageName("1000-simplify-qem-abcde");
    CHECK(variant.has_value());
    CHECK_EQ(variant->number, std::uint64_t{1000});
    CHECK_EQ(variant->command, std::string("simplify-qem"));
    CHECK_EQ(variant->text(), std::string("1000-simplify-qem-abcde"));
    // A killed run's leftover, and other names.
    CHECK(!chain::parseStageName("001-simplify-7xd2m.meshtinker-0123456789abcdef.tmp"));
    CHECK(!chain::parseStageName("01-simplify-7xd2m"));
    CHECK(!chain::parseStageName("001-simplify-7XD2M"));
    CHECK(!chain::parseStageName("001--7xd2m"));
    CHECK(!chain::parseStageName("001-7xd2m"));
    CHECK(!chain::parseStageName("meshtinker-1234-abcdef"));
    const std::string id = chain::randomId();
    CHECK_EQ(id.size(), std::size_t{5});
}

TEST_CASE(creates_a_chain_and_adds_stages) {
    const fs::path work = test::workDirectory("chain/create") / "work";
    {
        common::Outputs outputs;
        auto stage = chain::StageWriter::create(work, "dir/foo.glb", "import", outputs);
        CHECK(fs::is_directory(work));
        CHECK(!stage->parentModel().has_value());
        CHECK_EQ(stage->stem(), std::string("foo"));
        stage->lossy().addRound("imageEncoding", Json{{"image", 0}, {"format", "image/jpeg"}}, nullptr);
        // Not a chain until chain.json is published, after the stage.
        CHECK(!chain::isChain(work));
        writeStage(*stage, outputs, "zero");
    }
    CHECK(chain::isChain(work));
    const chain::Chain chain = chain::Chain::open(work);
    CHECK_EQ(chain.source(), std::string("foo.glb"));
    const chain::StageName first = chain.last();
    CHECK_EQ(first.number, std::uint64_t{0});
    CHECK_EQ(first.command, std::string("import"));
    CHECK(fs::exists(chain.model(first)));
    const Json record = chain.record(first);
    CHECK(record["parent"].is_null());
    CHECK_EQ(record["stage"].get<std::string>(), first.text());
    CHECK_EQ(record["report"]["answer"].get<int>(), 42);
    CHECK_EQ(record["argv"][1].get<std::string>(), std::string("work"));
    CHECK_EQ(record["lossy"]["imageEncoding"][0]["stage"].get<std::string>(), first.text());

    common::Diagnostics diagnostics;
    {
        common::Outputs outputs;
        auto stage = chain::StageWriter::add(chain, "simplify", outputs);
        CHECK_EQ(stage->parentModel()->filename().string(), std::string("foo.gltf"));
        CHECK_EQ(stage->name().number, std::uint64_t{1});
        stage->lossy().addError("simplificationError", 0.001);
        stage->lossy().addRound("imageEncoding", Json{{"image", 0}, {"format", "image/ktx2"}}, &diagnostics);
        writeStage(*stage, outputs, "one");
    }
    CHECK_EQ(diagnostics.entries().size(), std::size_t{1});
    CHECK_EQ(diagnostics.entries()[0].code, std::string("repeated-lossy-image-encoding"));
    {
        common::Outputs outputs;
        auto stage = chain::StageWriter::add(chain, "simplify-qem", outputs);
        stage->lossy().addError("simplificationError", 0.002);
        // The first round of its kind: no warning.
        stage->lossy().addRound("geometryQuantization", Json{{"positionBits", 14}}, &diagnostics);
        writeStage(*stage, outputs, "two");
    }
    CHECK_EQ(diagnostics.entries().size(), std::size_t{1});
    const chain::StageName last = chain.last();
    CHECK_EQ(last.number, std::uint64_t{2});
    CHECK_EQ(last.command, std::string("simplify-qem"));
    const Json lossy = chain.record(last)["lossy"];
    CHECK(std::abs(lossy["simplificationError"].get<double>() - 0.003) < 1e-12);
    CHECK_EQ(lossy["imageEncoding"].size(), std::size_t{2});
    CHECK_EQ(chain.record(last)["parent"].get<std::string>(), chain.stages()[1].text());
}

TEST_CASE(finds_the_last_stage_by_number) {
    const fs::path work = test::workDirectory("chain/last");
    writeText(work / "chain.json", R"({"format": 1, "source": "a.gltf"})");
    for (const char* name : {"999-a-aaaaa", "1000-b-bbbbb", "002-c-ccccc"}) {
        fs::create_directory(work / name);
        writeText(work / name / "stage.json", R"({"format": 1})");
    }
    // Not stages: a killed run's leftover, no stage.json, a workspace.
    fs::create_directory(work / "1001-d-ddddd.meshtinker-0123456789abcdef.tmp");
    writeText(work / "1001-d-ddddd.meshtinker-0123456789abcdef.tmp" / "stage.json", "{}");
    fs::create_directory(work / "1002-e-eeeee");
    fs::create_directory(work / "meshtinker-12-0123456789abcdef");
    const chain::Chain chain = chain::Chain::open(work);
    CHECK_EQ(chain.stages().size(), std::size_t{3});
    CHECK_EQ(chain.last().text(), std::string("1000-b-bbbbb"));
}

TEST_CASE(refuses_what_it_cannot_read) {
    const fs::path root = test::workDirectory("chain/refused");
    fs::create_directory(root / "plain");
    CHECK_EQ(failureCode([&] { chain::Chain::open(root / "plain"); }), std::string("not-a-chain"));
    fs::create_directory(root / "newer");
    writeText(root / "newer" / "chain.json", R"({"format": 2, "source": "a.gltf"})");
    CHECK_EQ(failureCode([&] { chain::Chain::open(root / "newer"); }), std::string("chain-format-newer"));
    fs::create_directory(root / "broken");
    writeText(root / "broken" / "chain.json", "{");
    CHECK_EQ(failureCode([&] { chain::Chain::open(root / "broken"); }), std::string("invalid-chain"));
    fs::create_directory(root / "empty");
    writeText(root / "empty" / "chain.json", R"({"format": 1, "source": "a.gltf"})");
    CHECK_EQ(failureCode([&] { chain::Chain::open(root / "empty").last(); }), std::string("chain-empty"));
    fs::create_directory(root / "empty" / "000-import-aaaaa");
    writeText(root / "empty" / "000-import-aaaaa" / "stage.json", R"({"format": 3})");
    const chain::Chain chain = chain::Chain::open(root / "empty");
    CHECK_EQ(failureCode([&] { chain.record(chain.last()); }), std::string("chain-format-newer"));
}

TEST_CASE(one_writer_at_a_time) {
    const fs::path work = test::workDirectory("chain/lock") / "work";
    {
        common::Outputs outputs;
        auto stage = chain::StageWriter::create(work, "a.gltf", "import", outputs);
        writeStage(*stage, outputs, "zero");
    }
    const chain::Chain chain = chain::Chain::open(work);
    common::Outputs outputs;
    auto first = chain::StageWriter::add(chain, "simplify", outputs);
    // flock locks open files, so a second open conflicts even in one process.
    common::Outputs other;
    CHECK_EQ(failureCode([&] { chain::StageWriter::add(chain, "compress", other); }), std::string("chain-in-use"));
}

TEST_CASE(failed_creation_leaves_nothing) {
    const fs::path root = test::workDirectory("chain/failed");
    {
        common::Outputs outputs;
        auto stage = chain::StageWriter::create(root / "work", "a.gltf", "import", outputs);
        common::OutputFile file = outputs.create(stage->directory() / "a.gltf");
        file.write(std::string("partial"));
        file.close();
    }
    CHECK(!fs::exists(root / "work"));
    // An existing empty directory is used, and stays.
    fs::create_directory(root / "mounted");
    {
        common::Outputs outputs;
        auto stage = chain::StageWriter::create(root / "mounted", "a.gltf", "import", outputs);
    }
    CHECK(fs::is_directory(root / "mounted"));
    CHECK(fs::is_empty(root / "mounted"));
    // A directory with files is refused, and so is a missing parent.
    writeText(root / "mounted" / "mine.txt", "mine");
    common::Outputs outputs;
    CHECK_EQ(failureCode([&] { chain::StageWriter::create(root / "mounted", "a.gltf", "import", outputs); }),
             std::string("output-exists"));
    CHECK_EQ(failureCode([&] { chain::StageWriter::create(root / "no" / "work", "a.gltf", "import", outputs); }),
             std::string("output-directory-missing"));
}
