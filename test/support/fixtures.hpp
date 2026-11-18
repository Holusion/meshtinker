#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace meshtinker::test {

struct Fixture {
    // The folder name in test/data, such as "triangle".
    std::string name;
    std::filesystem::path model;
    // The fixture's expected.json, authoritative (docs/validation.md).
    nlohmann::json expected;
};

// Every fixture listed in test/corpus.json, in order.
const std::vector<Fixture>& fixtures();
const Fixture& fixture(const char* name);

// A fresh, empty directory under the build tree for a test's files.
std::filesystem::path workDirectory(const std::string& name);

} // namespace meshtinker::test
