#include "commands/normals/normals.hpp"
#include "commands/weld/weld.hpp"

#include "meshtinker/gltf/document.hpp"
#include "meshtinker/gltf/geometry.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace meshtinker;
using gltf::Document;

namespace {

struct Run {
    normals::Report report;
    common::Diagnostics diagnostics;
    fs::path model;
};

Run run(const fs::path& model, const std::string& name, const normals::Options& options = {}) {
    const fs::path directory = test::workDirectory("normals/" + name);
    Run result;
    common::Workspace workspace(directory, false);
    common::Outputs outputs;
    result.report = normals::normals(model, directory, name, options, workspace, outputs, result.diagnostics);
    outputs.commit();
    result.model = directory / (name + ".gltf");
    return result;
}

using Vec3 = std::array<float, 3>;

std::vector<Vec3> read3(const cgltf_accessor& accessor) {
    std::vector<Vec3> values(accessor.count);
    for (std::size_t v = 0; v < accessor.count; ++v) {
        cgltf_accessor_read_float(&accessor, v, values[v].data(), 3);
    }
    return values;
}

const cgltf_mesh& mesh(const cgltf_data& data, const std::string& name) {
    for (std::size_t m = 0; m < data.meshes_count; ++m) {
        if (data.meshes[m].name != nullptr && name == data.meshes[m].name) {
            return data.meshes[m];
        }
    }
    throw std::runtime_error("no mesh " + name);
}

const cgltf_accessor* attribute(const cgltf_primitive& primitive, cgltf_attribute_type type) {
    return gltf::findAttribute(primitive, type, 0);
}

std::array<std::uint32_t, 3> bits(const Vec3& v) {
    std::array<std::uint32_t, 3> result{};
    std::memcpy(result.data(), v.data(), sizeof(result));
    return result;
}

// The normals found at each position, over every primitive of `data`, as
// their bits: one per position when nothing creases.
std::map<std::array<std::uint32_t, 3>, std::map<std::array<std::uint32_t, 3>, int>> normalsByPosition(const cgltf_data& data) {
    std::map<std::array<std::uint32_t, 3>, std::map<std::array<std::uint32_t, 3>, int>> result;
    for (std::size_t m = 0; m < data.meshes_count; ++m) {
        for (std::size_t p = 0; p < data.meshes[m].primitives_count; ++p) {
            const cgltf_primitive& primitive = data.meshes[m].primitives[p];
            const cgltf_accessor* normal = attribute(primitive, cgltf_attribute_type_normal);
            if (normal == nullptr) {
                continue;
            }
            const std::vector<Vec3> positions = read3(*attribute(primitive, cgltf_attribute_type_position));
            const std::vector<Vec3> normals = read3(*normal);
            for (std::size_t v = 0; v < positions.size(); ++v) {
                ++result[bits(positions[v])][bits(normals[v])];
            }
        }
    }
    return result;
}

double length(const Vec3& v) { return std::sqrt(double{v[0]} * v[0] + double{v[1]} * v[1] + double{v[2]} * v[2]); }

// The smallest cosine between a vertex's normal and the unit normal of a
// face it belongs to, the face's normal following its winding.
double worstFaceAgreement(const cgltf_primitive& primitive, const std::vector<Vec3>& positions,
                          const std::vector<Vec3>& normals) {
    double worst = 1;
    for (std::size_t t = 0; t + 2 < primitive.indices->count; t += 3) {
        std::array<std::size_t, 3> v{};
        for (std::size_t k = 0; k < 3; ++k) {
            v[k] = cgltf_accessor_read_index(primitive.indices, t + k);
        }
        Vec3 e1{};
        Vec3 e2{};
        for (std::size_t k = 0; k < 3; ++k) {
            e1[k] = positions[v[1]][k] - positions[v[0]][k];
            e2[k] = positions[v[2]][k] - positions[v[0]][k];
        }
        const Vec3 face{e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        for (const std::size_t vertex : v) {
            const Vec3& n = normals[vertex];
            worst = std::min(worst, (double{n[0]} * face[0] + double{n[1]} * face[1] + double{n[2]} * face[2]) / length(face));
        }
    }
    return worst;
}

} // namespace

// A scan without normals gets unit normals pointing out of its surface, one
// value per position: its UV seams leave no crease, and no vertex is split.
// Unlit primitives are left without; morph targets are copied, with a warning.
TEST_CASE(missing_normals_are_written_smooth) {
    const test::Fixture& fixture = test::fixture("bare");
    const Run result = run(fixture.model, "bare");
    CHECK_EQ(result.report.added, std::uint64_t{1});
    CHECK_EQ(result.report.unlit, std::uint64_t{1});
    CHECK_EQ(result.report.skipped, std::uint64_t{1});
    CHECK_EQ(result.diagnostics.toJson()[0]["code"].get<std::string>(), std::string("morph-targets-kept"));
    const Document output = Document::read(result.model);
    const cgltf_data& data = output.data();
    const cgltf_primitive& scan = mesh(data, "Scan").primitives[0];
    const std::vector<Vec3> positions = read3(*attribute(scan, cgltf_attribute_type_position));
    const std::vector<Vec3> normals = read3(*attribute(scan, cgltf_attribute_type_normal));
    CHECK_EQ(normals.size(), fixture.expected["normals"]["vertices"]["Scan"].get<std::size_t>());
    for (const Vec3& normal : normals) {
        CHECK(std::abs(length(normal) - 1) < 1e-5);
    }
    // Each normal is on the front side of every face around it, as the
    // winding defines it (counter-clockwise, spec 3.7.2.1), and close to it:
    // the scan has no sharp edge.
    CHECK(worstFaceAgreement(scan, positions, normals) > 0.5);
    for (const auto& [position, found] : normalsByPosition(data)) {
        CHECK_EQ(found.size(), std::size_t{1});
    }
    CHECK(attribute(mesh(data, "Unlit").primitives[0], cgltf_attribute_type_normal) == nullptr);
    CHECK(attribute(mesh(data, "Morph").primitives[0], cgltf_attribute_type_normal) == nullptr);
}

// Flat normals on tiles are replaced by smooth ones that agree across the
// tiles' borders, and their tangents are dropped. Each tile then welds back
// to the indexed tile it was unrolled from.
TEST_CASE(flat_tiles_are_smoothed_across_borders) {
    const test::Fixture& fixture = test::fixture("flat-tiles");
    const Run result = run(fixture.model, "flat-tiles");
    CHECK_EQ(result.report.smoothed, std::uint64_t{4});
    CHECK_EQ(result.report.output.vertices, result.report.input.vertices);
    const Document output = Document::read(result.model);
    std::size_t shared = 0;
    for (const auto& [position, found] : normalsByPosition(output.data())) {
        CHECK_EQ(found.size(), std::size_t{1});
        shared += found.begin()->second > 1 ? 1U : 0U;
    }
    CHECK(shared > 0);
    const cgltf_mesh& tiles = output.data().meshes[0];
    for (std::size_t p = 0; p < tiles.primitives_count; ++p) {
        CHECK(attribute(tiles.primitives[p], cgltf_attribute_type_tangent) == nullptr);
    }

    const fs::path directory = test::workDirectory("normals/flat-tiles-welded");
    common::Workspace workspace(directory, false);
    common::Outputs outputs;
    common::Diagnostics diagnostics;
    weld::weld(result.model, directory, "welded", weld::Options{}, workspace, outputs, diagnostics);
    outputs.commit();
    const Document welded = Document::read(directory / "welded.gltf");
    const cgltf_mesh& after = welded.data().meshes[0];
    CHECK_EQ(after.primitives_count, std::size_t{4});
    for (std::size_t p = 0; p < after.primitives_count; ++p) {
        CHECK_EQ(attribute(after.primitives[p], cgltf_attribute_type_position)->count,
                 fixture.expected["normals"]["weldedVertices"][p].get<std::size_t>());
    }
}

// The cube's 90° edges are sharper than the crease angle: every normal is its
// side's axis. Without creases, a corner's three sides meet at equal angles,
// so its normal is the diagonal.
TEST_CASE(crease_angle_keeps_box_edges) {
    const fs::path cube = test::fixture("cube").model;
    const Run creased = run(cube, "cube");
    CHECK_EQ(creased.report.added, std::uint64_t{2});
    const Document output = Document::read(creased.model);
    for (const auto& [position, found] : normalsByPosition(output.data())) {
        for (const auto& [normal, count] : found) {
            Vec3 n{};
            std::memcpy(n.data(), normal.data(), sizeof(n));
            const float largest = std::max({std::abs(n[0]), std::abs(n[1]), std::abs(n[2])});
            CHECK(largest == 1.0F);
        }
    }

    normals::Options smooth;
    smooth.crease_angle = 180;
    const Run rounded = run(cube, "cube-smooth", smooth);
    const Document round = Document::read(rounded.model);
    const auto corner = normalsByPosition(round.data()).at(bits({1, 1, 1}));
    CHECK_EQ(corner.size(), std::size_t{1});
    Vec3 n{};
    std::memcpy(n.data(), corner.begin()->first.data(), sizeof(n));
    for (const float component : n) {
        CHECK(std::abs(component - 1 / std::sqrt(3.0F)) < 1e-6F);
    }
}

// Authored normals are kept: smooth ones on the soup, and flat ones with
// --keep-flat. Smoothed normals aren't flat any more, so a second run keeps
// them.
TEST_CASE(authored_normals_are_kept) {
    const Run soup = run(test::fixture("soup").model, "soup");
    CHECK_EQ(soup.report.kept, std::uint64_t{1});
    CHECK_EQ(soup.report.input.primitives, std::uint64_t{0});

    normals::Options keep;
    keep.keep_flat = true;
    const Run flat = run(test::fixture("flat-tiles").model, "flat-tiles-kept", keep);
    CHECK_EQ(flat.report.kept, std::uint64_t{4});
    CHECK_EQ(flat.report.input.primitives, std::uint64_t{0});

    const Run first = run(test::fixture("flat-tiles").model, "flat-tiles-first");
    const Run second = run(first.model, "flat-tiles-second");
    CHECK_EQ(second.report.kept, std::uint64_t{4});
    CHECK_EQ(second.report.input.primitives, std::uint64_t{0});
}
