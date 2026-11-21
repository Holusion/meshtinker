#include "commands/weld/weld.hpp"

#include "meshtinker/gltf/document.hpp"
#include "meshtinker/gltf/geometry.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace meshtinker;
using gltf::Document;

namespace {

struct Run {
    weld::Report report;
    common::Diagnostics diagnostics;
    fs::path model;
};

Run run(const fs::path& model, const std::string& name) {
    const fs::path directory = test::workDirectory("weld/" + name);
    Run result;
    common::Workspace workspace(directory, false);
    common::Outputs outputs;
    result.report = weld::weld(model, directory, name, weld::Options{}, workspace, outputs, result.diagnostics);
    outputs.commit();
    result.model = directory / (name + ".gltf");
    return result;
}

Run run(const std::string& fixture) { return run(test::fixture(fixture.c_str()).model, fixture); }

using Values = std::vector<float>;

// The values one accessor holds for each vertex, as floats.
std::vector<Values> read(const cgltf_accessor& accessor) {
    const std::size_t n = cgltf_num_components(accessor.type);
    std::vector<Values> values(accessor.count, Values(n));
    for (std::size_t v = 0; v < accessor.count; ++v) {
        cgltf_accessor_read_float(&accessor, v, values[v].data(), n);
    }
    return values;
}

// Every corner a triangle list draws, as all its values: attributes in name
// order, then each morph target's.
std::vector<Values> corners(const cgltf_primitive& primitive) {
    std::map<std::string, std::vector<Values>> attributes;
    for (std::size_t a = 0; a < primitive.attributes_count; ++a) {
        attributes[primitive.attributes[a].name] = read(*primitive.attributes[a].data);
    }
    std::vector<std::vector<Values>> streams;
    streams.reserve(attributes.size());
    for (const auto& entry : attributes) {
        streams.push_back(entry.second);
    }
    for (std::size_t t = 0; t < primitive.targets_count; ++t) {
        for (std::size_t a = 0; a < primitive.targets[t].attributes_count; ++a) {
            streams.push_back(read(*primitive.targets[t].attributes[a].data));
        }
    }
    const std::size_t count = gltf::cornerCount(primitive);
    std::vector<Values> result(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t vertex = primitive.indices != nullptr ? cgltf_accessor_read_index(primitive.indices, i) : i;
        for (const std::vector<Values>& stream : streams) {
            result[i].insert(result[i].end(), stream[vertex].begin(), stream[vertex].end());
        }
    }
    return result;
}

std::size_t vertexCount(const cgltf_primitive& primitive) {
    return gltf::findAttribute(primitive, cgltf_attribute_type_position, 0)->count;
}

} // namespace

// Each primitive keeps one vertex per kind: as many as expected.json says,
// and every corner draws the values it drew before, in the same order. −0
// and 0 merge; a morph target keeps two vertices apart.
TEST_CASE(soup_welds_to_its_distinct_vertices) {
    const test::Fixture& fixture = test::fixture("soup");
    const Run result = run("soup");
    CHECK_EQ(result.report.welded, std::uint64_t{2});
    CHECK_EQ(result.report.output.faces, result.report.input.faces);
    const Document input = Document::read(fixture.model);
    const Document output = Document::read(result.model);
    const cgltf_data& in = input.data();
    const cgltf_data& out = output.data();
    CHECK_EQ(out.meshes_count, in.meshes_count);
    for (std::size_t m = 0; m < out.meshes_count; ++m) {
        const cgltf_primitive& before = in.meshes[m].primitives[0];
        const cgltf_primitive& after = out.meshes[m].primitives[0];
        CHECK_EQ(vertexCount(after), fixture.expected["weld"]["vertices"][out.meshes[m].name].get<std::size_t>());
        CHECK(corners(after) == corners(before));
        CHECK_EQ(after.targets_count, before.targets_count);
    }
}

// Welding a welded model changes nothing.
TEST_CASE(welding_twice_changes_nothing) {
    const Run first = run("soup");
    const Run second = run(first.model, "soup-again");
    CHECK_EQ(second.report.welded, std::uint64_t{0});
    CHECK_EQ(second.report.unchanged, std::uint64_t{2});
    CHECK_EQ(second.report.output.vertices, first.report.output.vertices);
}

// Draco and meshopt primitives are copied as they are, without a warning:
// their encoders already merged their vertices.
TEST_CASE(compressed_primitives_are_copied) {
    for (const char* fixture : {"draco", "meshopt", "meshopt-quantized"}) {
        const Run result = run(fixture);
        CHECK_EQ(result.report.compressed, std::uint64_t{1});
        CHECK_EQ(result.report.welded, std::uint64_t{0});
        CHECK(result.diagnostics.toJson().empty());
        const Document output = Document::read(result.model);
        CHECK(output.json().contains("extensionsUsed"));
    }
}

// Primitives that share vertices are copied, with a warning: welding one
// would give it a copy of its own.
TEST_CASE(shared_vertices_are_copied) {
    const Run result = run("shared");
    CHECK_EQ(result.report.skipped, std::uint64_t{2});
    CHECK_EQ(result.diagnostics.toJson()[0]["code"].get<std::string>(), std::string("shared-vertices-kept"));
}

// Points and lines are copied, with a warning.
TEST_CASE(points_and_lines_are_copied) {
    const Run result = run("modes");
    CHECK_EQ(result.report.skipped, std::uint64_t{4});
    CHECK_EQ(result.report.input.primitives, std::uint64_t{3});
    CHECK_EQ(result.diagnostics.toJson()[0]["code"].get<std::string>(), std::string("points-and-lines-kept"));
}
