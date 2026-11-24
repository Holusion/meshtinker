// `inspect` figures against each fixture's expected.json, which is
// authoritative (docs/validation.md), and `import` against `inspect`: the
// report import prints must be what inspecting its output gives.

#include "meshtinker/analysis/analyze.hpp"
#include "meshtinker/analysis/pipeline.hpp"
#include "meshtinker/common/failure.hpp"
#include "meshtinker/common/outputs.hpp"
#include "meshtinker/gltf/layout.hpp"
#include "meshtinker/gltf/writer.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <algorithm>
#include <cmath>

namespace fs = std::filesystem;
using namespace meshtinker;
using nlohmann::json;

namespace {

bool hasDiagnostic(const common::Diagnostics& diagnostics, const std::string& code) {
    const auto& entries = diagnostics.entries();
    return std::any_of(entries.begin(), entries.end(), [&](const common::Diagnostic& d) { return d.code == code; });
}

// Whether the fixture's model may be read: reference fixtures need --trusted.
bool trustedFor(const test::Fixture& f) { return f.name == "refs-outside" || f.name == "refs-symlink"; }

void checkFixture(const test::Fixture& f) {
    const json& expected = f.expected;
    common::Workspace workspace(test::workDirectory("fixtures/" + f.name), false);
    common::Diagnostics diagnostics;
    const analysis::Inspection inspection = analysis::inspect(f.model, {trustedFor(f)}, workspace, diagnostics);
    const json report = analysis::toJson(inspection.report, json::object(), diagnostics);

    if (expected.contains("geometry")) {
        for (const auto& item : expected["geometry"].items()) {
            CHECK_EQ(report["geometry"][item.key()], item.value());
        }
    }
    if (expected.contains("scene")) {
        for (const auto& item : expected["scene"].items()) {
            CHECK_EQ(report["scene"][item.key()], item.value());
        }
    }
    if (expected.contains("firstNamedRootNode")) {
        CHECK_EQ(report["scene"]["firstNamedRootNode"], expected["firstNamedRootNode"]);
    }
    if (expected.contains("bounds")) {
        const json& bounds = report["scene"]["bounds"];
        if (expected["bounds"].is_null()) {
            CHECK(bounds.is_null());
        } else if (bounds.is_null()) {
            test::fail("bounds are null", __FILE__, __LINE__);
        } else {
            const double tolerance = expected["bounds"].value("tolerance", 1e-9);
            for (const char* side : {"min", "max"}) {
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    const double actual = bounds[side][axis].get<double>();
                    const double wanted = expected["bounds"][side][axis].get<double>();
                    if (std::abs(actual - wanted) > tolerance) {
                        test::fail(std::string("bounds.") + side + "[" + std::to_string(axis) + "] is " +
                                       std::to_string(actual) + ", expected " + std::to_string(wanted),
                                   __FILE__, __LINE__);
                    }
                }
            }
        }
    }
    if (expected.contains("images")) {
        const json& images = expected["images"];
        for (std::size_t i = 0; i < images.size(); ++i) {
            const auto found = std::find_if(report["images"].begin(), report["images"].end(),
                                            [&](const json& image) { return image["index"] == i; });
            // An image the profile drops is not listed.
            if (images[i].value("dropped", false)) {
                CHECK(found == report["images"].end());
                continue;
            }
            CHECK(found != report["images"].end());
            if (found == report["images"].end()) {
                continue;
            }
            for (const char* key : {"mimeType", "codec", "width", "height"}) {
                if (images[i].contains(key)) {
                    CHECK_EQ((*found)[key], images[i][key]);
                }
            }
            if (images[i].contains("occupancy")) {
                const json& wanted = images[i]["occupancy"];
                const json& actual = (*found)["occupancy"];
                if (wanted.is_null()) {
                    CHECK(actual.is_null());
                } else if (actual.is_null()) {
                    test::fail("image " + std::to_string(i) + " occupancy is null", __FILE__, __LINE__);
                } else if (std::abs(actual.get<double>() - wanted.get<double>()) > 1e-12) {
                    test::fail("image " + std::to_string(i) + " occupancy is " + actual.dump() + ", expected " +
                                   wanted.dump(),
                               __FILE__, __LINE__);
                }
            }
        }
    }
    if (expected.contains("diagnostics")) {
        for (const json& code : expected["diagnostics"]) {
            CHECK(hasDiagnostic(diagnostics, code.get<std::string>()));
        }
    }
}

// import's report against inspecting what it wrote.
void checkImport(const test::Fixture& f) {
    const fs::path directory = test::workDirectory("import/" + f.name);
    common::Workspace workspace(directory, false);
    common::Diagnostics diagnostics;
    analysis::Inspection inspection = analysis::inspect(f.model, {trustedFor(f)}, workspace, diagnostics);
    const gltf::Layout layout = gltf::normalizedLayout(inspection.document, inspection.selection, diagnostics);
    common::Outputs outputs;
    const auto files = gltf::writeSeparate(inspection.document, layout, directory, f.name, outputs);
    outputs.commit();
    const analysis::Report imported =
        analysis::outputReport(inspection.report, layout, files, gltf::imageFileNames(inspection.document, f.name));

    common::Diagnostics again;
    common::Workspace second(directory, false);
    const analysis::Inspection output = analysis::inspect(directory / (f.name + ".gltf"), {}, second, again);
    json a = analysis::toJson(imported, json::object(), diagnostics);
    json b = analysis::toJson(output.report, json::object(), again);
    a.erase("diagnostics");
    b.erase("diagnostics");
    if (a != b) {
        test::fail("import report differs from inspecting its output:\n  import:  " + a.dump() + "\n  inspect: " + b.dump(),
                   __FILE__, __LINE__);
    }
    // The output holds one scene, numbered 0, and nothing only other scenes reached.
    if (output.report.scene) {
        CHECK_EQ(output.report.scene->index, 0u);
        CHECK_EQ(output.document.data().scenes_count, std::size_t{1});
    }
}

// Fixtures whose expected.json names an `error` must be rejected with that code.
bool rejected(const test::Fixture& f) { return f.expected.contains("error") || f.name == "refs-absolute"; }
bool readable(const test::Fixture& f) { return !rejected(f); }

} // namespace

TEST_CASE(inspect_matches_expected) {
    for (const test::Fixture& f : test::fixtures()) {
        if (!readable(f)) {
            continue;
        }
        const int before = test::failureCount();
        checkFixture(f);
        if (test::failureCount() > before) {
            std::fprintf(stderr, "  in fixture %s\n", f.name.c_str());
        }
    }
}

TEST_CASE(rejected_fixtures_fail_with_their_code) {
    for (const test::Fixture& f : test::fixtures()) {
        if (!f.expected.contains("error")) {
            continue;
        }
        common::Workspace workspace(test::workDirectory("fixtures/" + f.name), false);
        common::Diagnostics diagnostics;
        try {
            analysis::inspect(f.model, {trustedFor(f)}, workspace, diagnostics);
            test::fail(f.name + " was accepted", __FILE__, __LINE__);
        } catch (const common::Failure& failure) {
            CHECK_EQ(failure.code(), f.expected["error"].get<std::string>());
            CHECK(failure.exitCode() == common::ExitCode::input_rejected);
        }
    }
}

TEST_CASE(import_reports_what_it_wrote) {
    for (const test::Fixture& f : test::fixtures()) {
        if (!readable(f)) {
            continue;
        }
        const int before = test::failureCount();
        checkImport(f);
        if (test::failureCount() > before) {
            std::fprintf(stderr, "  in fixture %s\n", f.name.c_str());
        }
    }
}
