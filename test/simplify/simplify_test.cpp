#include "commands/simplify/simplify.hpp"

#include "meshtinker/gltf/document.hpp"
#include "meshtinker/gltf/geometry.hpp"
#include "meshtinker/gltf/layout.hpp"
#include "meshtinker/gltf/writer.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <array>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

namespace fs = std::filesystem;
using namespace meshtinker;
using gltf::Document;

namespace {

struct Run {
    simplify::Report report;
    common::Diagnostics diagnostics;
    fs::path model;
};

Run run(const std::string& fixture, double error = simplify::default_error) {
    const test::Fixture& f = test::fixture(fixture.c_str());
    const fs::path directory = test::workDirectory("simplify/" + fixture + "-" + std::to_string(error));
    Run result;
    simplify::Options options;
    options.error = error;
    common::Workspace workspace(directory, false);
    common::Outputs outputs;
    result.report = simplify::simplify(f.model, directory, fixture, options, workspace, outputs, result.diagnostics);
    outputs.commit();
    result.model = directory / (fixture + ".gltf");
    return result;
}

using Vertex = std::vector<float>;

// Every vertex of a primitive, as all its attribute values in attribute-name order.
std::vector<Vertex> vertices(const cgltf_primitive& primitive) {
    std::map<std::string, const cgltf_accessor*> attributes;
    for (std::size_t a = 0; a < primitive.attributes_count; ++a) {
        attributes[primitive.attributes[a].name] = primitive.attributes[a].data;
    }
    const std::size_t count = attributes.at("POSITION")->count;
    std::vector<Vertex> result(count);
    for (const auto& [name, accessor] : attributes) {
        const std::size_t n = cgltf_num_components(accessor->type);
        for (std::size_t v = 0; v < count; ++v) {
            float value[4] = {};
            cgltf_accessor_read_float(accessor, v, value, n);
            result[v].insert(result[v].end(), value, value + n);
        }
    }
    return result;
}

std::vector<std::array<float, 3>> positions(const cgltf_primitive& primitive) {
    const cgltf_accessor* accessor = gltf::findAttribute(primitive, cgltf_attribute_type_position, 0);
    std::vector<std::array<float, 3>> result(accessor->count);
    for (std::size_t v = 0; v < accessor->count; ++v) {
        cgltf_accessor_read_float(accessor, v, result[v].data(), 3);
    }
    return result;
}

std::vector<std::array<std::uint32_t, 3>> triangles(const cgltf_primitive& primitive) {
    std::vector<std::array<std::uint32_t, 3>> result(primitive.indices->count / 3);
    for (std::size_t t = 0; t < result.size(); ++t) {
        for (std::size_t k = 0; k < 3; ++k) {
            result[t][k] = static_cast<std::uint32_t>(cgltf_accessor_read_index(primitive.indices, 3 * t + k));
        }
    }
    return result;
}

using Point = std::array<double, 3>;

Point toPoint(const std::array<float, 3>& p) { return {p[0], p[1], p[2]}; }
Point sub(const Point& a, const Point& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
double dot(const Point& a, const Point& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Distance from p to triangle abc (Ericson, Real-Time Collision Detection, 5.1.5).
double distance(const Point& p, const Point& a, const Point& b, const Point& c) {
    const Point ab = sub(b, a);
    const Point ac = sub(c, a);
    const Point ap = sub(p, a);
    const double d1 = dot(ab, ap);
    const double d2 = dot(ac, ap);
    const auto length = [](const Point& v) { return std::sqrt(dot(v, v)); };
    const auto at = [&](double v, double w) {
        return length(sub(p, {a[0] + ab[0] * v + ac[0] * w, a[1] + ab[1] * v + ac[1] * w, a[2] + ab[2] * v + ac[2] * w}));
    };
    if (d1 <= 0 && d2 <= 0) {
        return length(ap);
    }
    const Point bp = sub(p, b);
    const double d3 = dot(ab, bp);
    const double d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) {
        return length(bp);
    }
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        return at(d1 / (d1 - d3), 0);
    }
    const Point cp = sub(p, c);
    const double d5 = dot(ab, cp);
    const double d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) {
        return length(cp);
    }
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        return at(0, d2 / (d2 - d6));
    }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return length(sub(p, {b[0] + (c[0] - b[0]) * w, b[1] + (c[1] - b[1]) * w, b[2] + (c[2] - b[2]) * w}));
    }
    const double denominator = 1 / (va + vb + vc);
    return at(vb * denominator, vc * denominator);
}

std::map<std::string, std::string> filesIn(const fs::path& directory) {
    std::map<std::string, std::string> files;
    for (const auto& entry : fs::directory_iterator(directory)) {
        std::ifstream in(entry.path(), std::ios::binary);
        files[entry.path().filename().u8string()] =
            std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    return files;
}

} // namespace

// Positions two primitives share in the input are in both after
// simplification: the borders between tiles stay sealed.
TEST_CASE(borders_between_primitives_stay_sealed) {
    const Document input = Document::read(test::fixture("scan-tiles").model);
    const Run result = run("scan-tiles", 0.01);
    const Document output = Document::read(result.model);
    CHECK_EQ(result.report.simplified, std::uint64_t{4});
    CHECK(result.report.output.faces < result.report.input.faces / 2);
    const cgltf_mesh& source = input.data().meshes[0];
    std::map<std::array<float, 3>, std::set<std::size_t>> owners;
    for (std::size_t p = 0; p < source.primitives_count; ++p) {
        for (const auto& position : positions(source.primitives[p])) {
            owners[position].insert(p);
        }
    }
    std::size_t shared = 0;
    const cgltf_mesh& simplified = output.data().meshes[0];
    std::vector<std::set<std::array<float, 3>>> kept(simplified.primitives_count);
    for (std::size_t p = 0; p < simplified.primitives_count; ++p) {
        for (const auto& position : positions(simplified.primitives[p])) {
            kept[p].insert(position);
        }
    }
    for (const auto& [position, primitives] : owners) {
        if (primitives.size() < 2) {
            continue;
        }
        ++shared;
        for (const std::size_t p : primitives) {
            CHECK(kept[p].count(position) == 1);
        }
    }
    CHECK(shared > 0);
}

// Simplification only removes vertices: each output vertex is an input
// vertex, with every attribute it had.
TEST_CASE(output_vertices_are_input_vertices) {
    const Document input = Document::read(test::fixture("scan-tiles").model);
    const Run result = run("scan-tiles", 0.01);
    const Document output = Document::read(result.model);
    for (std::size_t p = 0; p < output.data().meshes[0].primitives_count; ++p) {
        const std::vector<Vertex> before = vertices(input.data().meshes[0].primitives[p]);
        const std::set<Vertex> known(before.begin(), before.end());
        for (const Vertex& vertex : vertices(output.data().meshes[0].primitives[p])) {
            CHECK(known.count(vertex) == 1);
        }
    }
}

// Every input vertex lies close to the simplified surface. meshoptimizer's
// error is a quadric estimate, not a bound on the distance: on this fixture
// the farthest input vertex is 1.5 times the error away at 0.001, and 2.1
// times at 0.01 (docs/simplify.md). The slack catches scale mistakes.
TEST_CASE(input_stays_within_the_error_of_the_output) {
    for (const double error : {0.001, 0.01}) {
        const Document input = Document::read(test::fixture("scan-tiles").model);
        const Run result = run("scan-tiles", error);
        const Document output = Document::read(result.model);
        Point min{1e30, 1e30, 1e30};
        Point max{-1e30, -1e30, -1e30};
        for (std::size_t p = 0; p < input.data().meshes[0].primitives_count; ++p) {
            for (const auto& position : positions(input.data().meshes[0].primitives[p])) {
                for (std::size_t k = 0; k < 3; ++k) {
                    min[k] = std::min(min[k], static_cast<double>(position[k]));
                    max[k] = std::max(max[k], static_cast<double>(position[k]));
                }
            }
        }
        const double scale = std::max({max[0] - min[0], max[1] - min[1], max[2] - min[2]});
        double worst = 0;
        for (std::size_t p = 0; p < input.data().meshes[0].primitives_count; ++p) {
            const cgltf_primitive& simplified = output.data().meshes[0].primitives[p];
            const auto points = positions(simplified);
            const auto faces = triangles(simplified);
            for (const auto& position : positions(input.data().meshes[0].primitives[p])) {
                double nearest = 1e30;
                for (const auto& face : faces) {
                    nearest = std::min(nearest, distance(toPoint(position), toPoint(points[face[0]]),
                                                         toPoint(points[face[1]]), toPoint(points[face[2]])));
                }
                worst = std::max(worst, nearest / scale);
            }
        }
        CHECK(result.report.achieved_error <= error);
        CHECK(worst <= 2.5 * error);
        std::fprintf(stderr, "error %g: worst vertex distance %g of the mesh's size\n", error, worst);
    }
}

// A model with nothing to collapse is written as `import` writes it.
TEST_CASE(nothing_to_collapse_writes_the_import) {
    const Run result = run("triangle");
    CHECK_EQ(result.report.unchanged, std::uint64_t{1});
    CHECK_EQ(result.report.simplified, std::uint64_t{0});
    const Document source = Document::read(test::fixture("triangle").model);
    common::Diagnostics diagnostics;
    const fs::path imported = test::workDirectory("simplify/triangle-import");
    common::Outputs outputs;
    gltf::writeSeparate(source, gltf::normalizedLayout(source, gltf::selectScene(source), diagnostics), imported,
                        "triangle", outputs);
    outputs.commit();
    std::map<std::string, std::string> written = filesIn(result.model.parent_path());
    CHECK(written == filesIn(imported));
}

// A simplified Draco primitive is written plain, and the extension goes.
TEST_CASE(simplified_draco_is_written_plain) {
    const Run result = run("draco", 0.01);
    CHECK_EQ(result.report.simplified, std::uint64_t{1});
    const Document output = Document::read(result.model);
    CHECK(!output.json().contains("extensionsUsed"));
    CHECK(!output.data().meshes[0].primitives[0].has_draco_mesh_compression);
}

// Points and lines are copied, with a warning. Strips and fans are read as
// lists, with their winding.
TEST_CASE(points_and_lines_are_copied) {
    const Run result = run("modes", 0.5);
    CHECK_EQ(result.report.skipped, std::uint64_t{4});
    CHECK_EQ(result.report.simplified + result.report.unchanged, std::uint64_t{3});
    CHECK_EQ(result.diagnostics.toJson()[0]["code"].get<std::string>(), std::string("points-and-lines-kept"));
}
