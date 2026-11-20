// meshtinker-placeholder on the fixtures: where the placeholder sits, what
// it keeps of the surface, and that it doesn't depend on how the input is
// stored or how many threads build it (docs/placeholder.md).

#include "commands/placeholder/placeholder.hpp"
#include "commands/placeholder/voxel.hpp"

#include "meshtinker/analysis/pipeline.hpp"
#include "meshtinker/common/failure.hpp"
#include "meshtinker/gltf/layout.hpp"
#include "meshtinker/gltf/writer.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

namespace fs = std::filesystem;
using namespace meshtinker;
using placeholder::Triangle;
using placeholder::Vec3;

namespace {

// Fixtures with triangles the placeholder can be built from.
bool buildable(const test::Fixture& f) {
    return f.name != "refs-absolute" && f.name != "empty" && !f.expected.contains("error");
}

placeholder::Report build(const fs::path& input, const fs::path& directory, const std::string& stem,
                          placeholder::Options options = {}) {
    options.read.trusted = options.read.trusted || input.u8string().find("refs-") != std::string::npos;
    common::Workspace workspace(directory, false);
    common::Outputs outputs;
    common::Diagnostics diagnostics;
    placeholder::Report report = placeholder::build(input, directory, stem, options, workspace, outputs, diagnostics);
    outputs.commit();
    return report;
}

std::string contents(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

analysis::Report::Bounds worldBounds(const fs::path& model) {
    common::Workspace workspace(model.parent_path(), false);
    common::Diagnostics diagnostics;
    const analysis::Inspection inspection = analysis::inspect(model, {true}, workspace, diagnostics);
    const std::optional<analysis::Report::Scene>& scene = inspection.report.scene;
    if (!scene || !scene->bounds) {
        test::fail("no bounds for " + model.u8string(), __FILE__, __LINE__);
        return {};
    }
    return *scene->bounds;
}

void importTo(const fs::path& input, const fs::path& directory, const std::string& stem) {
    common::Diagnostics diagnostics;
    const gltf::Document source = gltf::Document::read(input, {true});
    const gltf::Layout layout = gltf::normalizedLayout(source, gltf::selectScene(source), diagnostics);
    common::Outputs outputs;
    gltf::writeSeparate(source, layout, directory, stem, outputs);
    outputs.commit();
}

std::uint64_t next(std::uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

// Deterministic triangles in [-2, 2]^3, so that failures reproduce.
std::vector<Triangle> randomTriangles(std::size_t count) {
    std::uint64_t state = 0x2545f4914f6cdd1dULL;
    const auto coordinate = [&] { return static_cast<double>(next(state) >> 11) / 9007199254740992.0 * 4 - 2; };
    std::vector<Triangle> triangles(count);
    for (Triangle& t : triangles) {
        for (Vec3& p : t) {
            p = {coordinate(), coordinate(), coordinate()};
        }
    }
    return triangles;
}

template <typename Visit>
void samples(const Triangle& t, Visit&& visit) {
    constexpr int n = 40;
    for (int i = 0; i <= n; ++i) {
        for (int j = 0; j <= n - i; ++j) {
            const double u = i / static_cast<double>(n);
            const double v = j / static_cast<double>(n);
            visit(Vec3{t[0][0] + (t[1][0] - t[0][0]) * u + (t[2][0] - t[0][0]) * v,
                       t[0][1] + (t[1][1] - t[0][1]) * u + (t[2][1] - t[0][1]) * v,
                       t[0][2] + (t[1][2] - t[0][2]) * u + (t[2][2] - t[0][2]) * v});
        }
    }
}

struct Output {
    std::vector<Vec3> positions;
    std::vector<std::array<std::uint32_t, 3>> triangles;
};

Output readOutput(const fs::path& path) {
    const gltf::Document document = gltf::Document::read(path);
    const cgltf_primitive& primitive = document.data().meshes[0].primitives[0];
    const cgltf_accessor* position = gltf::findAttribute(primitive, cgltf_attribute_type_position, 0);
    Output output;
    for (std::size_t v = 0; v < position->count; ++v) {
        float p[3] = {};
        cgltf_accessor_read_float(position, v, p, 3);
        output.positions.push_back({p[0], p[1], p[2]});
    }
    for (std::size_t i = 0; i + 2 < primitive.indices->count; i += 3) {
        output.triangles.push_back({static_cast<std::uint32_t>(cgltf_accessor_read_index(primitive.indices, i)),
                                    static_cast<std::uint32_t>(cgltf_accessor_read_index(primitive.indices, i + 1)),
                                    static_cast<std::uint32_t>(cgltf_accessor_read_index(primitive.indices, i + 2))});
    }
    return output;
}

void reportFixture(const test::Fixture& f, int failures_before) {
    if (test::failureCount() > failures_before) {
        std::fprintf(stderr, "  in fixture %s\n", f.name.c_str());
    }
}

} // namespace

TEST_CASE(overlap_never_misses_a_sampled_point) {
    for (const Triangle& t : randomTriangles(2000)) {
        bool inside = false;
        samples(t, [&](const Vec3& p) {
            inside = inside || (std::abs(p[0]) < 0.5 && std::abs(p[1]) < 0.5 && std::abs(p[2]) < 0.5);
        });
        CHECK(!inside || placeholder::overlaps({0, 0, 0}, t));
    }
}

TEST_CASE(overlap_is_rejected_far_away) {
    CHECK(!placeholder::overlaps({0, 0, 0}, Triangle{{{3, 3, 3}, {4, 3, 3}, {3, 4, 3}}}));
    // Every axis-aligned test passes for this one; only the plane or edge axes separate it.
    const Triangle t{{{1.5, -2, 0}, {-2, 1.5, 0}, {1.5, 1.5, 0}}};
    CHECK(!placeholder::overlaps({-0.9, -0.9, 0}, t));
    CHECK(placeholder::overlaps({1, 1, 0}, t));
}

TEST_CASE(closest_point_beats_every_sample) {
    const Vec3 point{0.3, -0.2, 0.7};
    const auto distance = [&](const Vec3& p) {
        return (p[0] - point[0]) * (p[0] - point[0]) + (p[1] - point[1]) * (p[1] - point[1]) +
               (p[2] - point[2]) * (p[2] - point[2]);
    };
    for (const Triangle& t : randomTriangles(500)) {
        const double best = distance(placeholder::closest(point, t));
        bool beaten = false;
        samples(t, [&](const Vec3& p) { beaten = beaten || distance(p) < best - 1e-9; });
        CHECK(!beaten);
    }
}

TEST_CASE(fixed_point_sums_keep_tiny_faces_and_their_position) {
    placeholder::Cell tiny;
    tiny.deposit({0.25, 0.5, 0.75}, 1e-12);
    CHECK(tiny.occupied());
    placeholder::Cell cell;
    for (int i = 0; i < 1000; ++i) {
        cell.deposit({0.25, 0.5, 0.75}, 1e-5);
    }
    const Vec3 mean = cell.mean();
    CHECK(std::abs(mean[0] - 0.25) < 1e-4 && std::abs(mean[1] - 0.5) < 1e-4 && std::abs(mean[2] - 0.75) < 1e-4);
}

TEST_CASE(lattice_is_cubic_and_padded) {
    const placeholder::Lattice lattice = placeholder::Lattice::around({0, 0, 0}, {10, 2.5, 0}, 4);
    CHECK_EQ(lattice.cell, 2.5);
    CHECK(lattice.dims == (std::array<std::size_t, 3>{6, 3, 3}));
    CHECK(lattice.origin == (Vec3{-2.5, -2.5, -2.5}));
    CHECK(lattice.coords(lattice.id(5, 2, 1)) == (std::array<std::size_t, 3>{5, 2, 1}));
    try {
        placeholder::Lattice::around({1, 1, 1}, {1, 1, 1}, 4);
        CHECK(false);
    } catch (const common::Failure& failure) {
        CHECK_EQ(failure.code(), std::string("no-extent"));
    }
}

// The cube's placeholder is closed (every edge shared by two faces, wound
// opposite ways), faces outward, fits the cube within a cell, and leaves out
// the cube hidden inside.
TEST_CASE(cube_is_closed_outward_and_hides_its_interior) {
    const fs::path directory = test::workDirectory("placeholder/cube");
    const placeholder::Report report = build(test::fixture("cube").model, directory, "cube");
    const Output output = readOutput(directory / "cube.gltf");
    const double cell = report.cell_size;

    std::map<std::pair<std::uint32_t, std::uint32_t>, int> edges;
    for (const auto& t : output.triangles) {
        for (std::size_t k = 0; k < 3; ++k) {
            ++edges[{t[k], t[(k + 1) % 3]}];
        }
        const Vec3& a = output.positions[t[0]];
        const Vec3& b = output.positions[t[1]];
        const Vec3& c = output.positions[t[2]];
        const Vec3 u{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const Vec3 v{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const Vec3 n{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        const Vec3 centroid{(a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3, (a[2] + b[2] + c[2]) / 3};
        CHECK(n[0] * centroid[0] + n[1] * centroid[1] + n[2] * centroid[2] > 0);
    }
    for (const auto& [edge, count] : edges) {
        CHECK_EQ(count, 1);
        const auto reverse = edges.find({edge.second, edge.first});
        CHECK(reverse != edges.end() && reverse->second == 1);
    }
    for (const Vec3& p : output.positions) {
        const double extent = std::max({std::abs(p[0]), std::abs(p[1]), std::abs(p[2])});
        CHECK(extent > 1 - cell && extent < 1 + cell);
    }
    CHECK_EQ(report.source_faces, 6U * 128U + 6U * 8U);
    CHECK_EQ(report.dropped_parts, 0U);
}

TEST_CASE(floaters_are_dropped_and_the_grid_refitted) {
    const fs::path directory = test::workDirectory("placeholder/floater");
    common::Workspace workspace(directory, false);
    common::Outputs outputs;
    common::Diagnostics diagnostics;
    const placeholder::Report report =
        placeholder::build(test::fixture("floater").model, directory, "floater", {}, workspace, outputs, diagnostics);
    outputs.commit();
    CHECK_EQ(report.dropped_parts, 1U);
    CHECK(report.passes > 1);
    CHECK(!diagnostics.empty() && diagnostics.entries().front().code == "small-parts-left-out");
    const analysis::Report::Bounds bounds = worldBounds(directory / "floater.gltf");
    for (std::size_t k = 0; k < 3; ++k) {
        CHECK(std::abs(bounds.min[k] + 1) < report.cell_size);
        CHECK(std::abs(bounds.max[k] - 1) < report.cell_size);
    }
}

// The placeholder overlays its source: its world bounds are within a cell
// of the source's (expected.json), whatever the transforms, instancing,
// skins, quantization or compression.
TEST_CASE(overlays_every_fixture) {
    for (const test::Fixture& f : test::fixtures()) {
        // `modes` also has points and lines, which the placeholder leaves out;
        // `floater` loses its floater, and `noise` its small cubes.
        if (!buildable(f) || !f.expected.contains("bounds") || f.name == "modes" || f.name == "floater" ||
            f.name == "noise") {
            continue;
        }
        const int before = test::failureCount();
        const fs::path directory = test::workDirectory("placeholder/overlay/" + f.name);
        const placeholder::Report report = build(f.model, directory, f.name);
        const analysis::Report::Bounds bounds = worldBounds(directory / (f.name + ".gltf"));
        const auto& expected = f.expected["bounds"];
        for (std::size_t k = 0; k < 3; ++k) {
            CHECK(std::abs(bounds.min[k] - expected["min"][k].get<double>()) <= report.cell_size);
            CHECK(std::abs(bounds.max[k] - expected["max"][k].get<double>()) <= report.cell_size);
        }
        reportFixture(f, before);
    }
}

// Normalizing in memory gives what importing first gives, byte for byte.
TEST_CASE(same_output_with_or_without_import) {
    for (const test::Fixture& f : test::fixtures()) {
        if (!buildable(f)) {
            continue;
        }
        const int before = test::failureCount();
        const fs::path direct = test::workDirectory("placeholder/direct/" + f.name);
        const fs::path imported = test::workDirectory("placeholder/imported/" + f.name);
        build(f.model, direct, "out");
        importTo(f.model, imported, f.name);
        build(imported / (f.name + ".gltf"), imported, "out");
        CHECK(contents(direct / "out.gltf") == contents(imported / "out.gltf"));
        CHECK(contents(direct / "out.bin") == contents(imported / "out.bin"));
        reportFixture(f, before);
    }
}

// The placeholder is written in the normalized profile: importing it changes
// nothing in its .bin, and drops nothing.
TEST_CASE(output_is_in_the_profile) {
    const fs::path directory = test::workDirectory("placeholder/profile");
    build(test::fixture("scan").model, directory, "out");
    const gltf::Document output = gltf::Document::read(directory / "out.gltf");
    const gltf::Selection selection = gltf::selectScene(output);
    for (std::size_t k = 0; k < gltf::kind_count; ++k) {
        CHECK_EQ(selection.dropped(static_cast<gltf::Kind>(k)), 0U);
    }
    const fs::path again = test::workDirectory("placeholder/profile-imported");
    importTo(directory / "out.gltf", again, "out");
    CHECK(contents(directory / "out.bin") == contents(again / "out.bin"));
}

// The frame's transforms are copied as written, and positions stay stored.
TEST_CASE(frame_transforms_are_copied) {
    const test::Fixture& f = test::fixture("scan");
    const fs::path directory = test::workDirectory("placeholder/frame");
    build(f.model, directory, "out");
    const gltf::Document source = gltf::Document::read(f.model);
    const gltf::Document output = gltf::Document::read(directory / "out.gltf");
    const gltf::Json& node = source.json()["nodes"][0];
    const gltf::Json& copied = output.json()["nodes"][0];
    for (const char* key : {"matrix", "translation", "rotation", "scale"}) {
        CHECK_EQ(copied.contains(key), node.contains(key));
        if (node.contains(key)) {
            CHECK(copied[key] == node[key]);
        }
    }
    CHECK_EQ(copied["name"].get<std::string>(), std::string("placeholder"));
}

TEST_CASE(thread_counts_agree) {
    common::Workspace workspace(test::workDirectory("placeholder/threads"), false);
    common::Diagnostics diagnostics;
    gltf::Document document = gltf::readNormalized(test::fixture("scan").model, {}, diagnostics);
    gltf::GeometryReader reader(document, workspace);
    const placeholder::Model model = placeholder::Model::read(document, reader, diagnostics);
    const placeholder::Voxelization one = placeholder::voxelize(model, 24, 1, 1);
    for (const unsigned threads : {2U, 3U, 7U}) {
        const placeholder::Voxelization many = placeholder::voxelize(model, 24, threads, 1);
        CHECK_EQ(many.grid.cells.size(), one.grid.cells.size());
        bool same = many.grid.cells.size() == one.grid.cells.size();
        for (std::size_t c = 0; same && c < one.grid.cells.size(); ++c) {
            same = many.grid.cells[c].area == one.grid.cells[c].area && many.grid.cells[c].sum == one.grid.cells[c].sum;
        }
        CHECK(same);
    }
}

TEST_CASE(a_scene_without_faces_is_rejected) {
    const fs::path directory = test::workDirectory("placeholder/empty");
    try {
        build(test::fixture("empty").model, directory, "out");
        CHECK(false);
    } catch (const common::Failure& failure) {
        CHECK_EQ(failure.code(), std::string("no-faces"));
        CHECK(failure.exitCode() == common::ExitCode::input_rejected);
    }
    CHECK(fs::is_empty(directory) || !fs::exists(directory / "out.gltf"));
}

TEST_CASE(existing_outputs_are_kept_and_partial_outputs_removed) {
    const fs::path directory = test::workDirectory("placeholder/existing");
    std::ofstream(directory / "out.bin") << "mine";
    try {
        build(test::fixture("triangle").model, directory, "out");
        CHECK(false);
    } catch (const common::Failure& failure) {
        CHECK_EQ(failure.code(), std::string("output-exists"));
    }
    CHECK(!fs::exists(directory / "out.gltf"));
    CHECK_EQ(contents(directory / "out.bin"), std::string("mine"));
}
