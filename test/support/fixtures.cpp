#include "support/fixtures.hpp"

#include <fstream>
#include <stdexcept>

namespace meshtinker::test {
namespace fs = std::filesystem;

namespace {

nlohmann::json readJson(const fs::path& path) {
    std::ifstream stream(path);
    if (!stream) {
        throw std::runtime_error("cannot open " + path.string());
    }
    return nlohmann::json::parse(stream);
}

} // namespace

const std::vector<Fixture>& fixtures() {
    static const std::vector<Fixture> all = [] {
        const fs::path data(MESHTINKER_TEST_DATA);
        const nlohmann::json corpus = readJson(data.parent_path() / "corpus.json");
        std::vector<Fixture> result;
        for (const auto& asset : corpus.at("assets")) {
            if (asset.at("source") != "fixture") {
                continue;
            }
            const std::string path = asset.at("path").get<std::string>();
            const std::string name = path.substr(0, path.find('/'));
            result.push_back({name, data / fs::u8path(path), readJson(data / name / "expected.json")});
        }
        return result;
    }();
    return all;
}

const Fixture& fixture(const char* name) {
    for (const Fixture& candidate : fixtures()) {
        if (candidate.name == name) {
            return candidate;
        }
    }
    throw std::runtime_error(std::string("no fixture named ") + name);
}

fs::path workDirectory(const std::string& name) {
    const fs::path directory = fs::path(MESHTINKER_BUILD_DIR) / "test-work" / name;
    fs::remove_all(directory);
    fs::create_directories(directory);
    return directory;
}

} // namespace meshtinker::test
