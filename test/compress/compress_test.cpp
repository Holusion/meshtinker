#include "commands/compress/compress.hpp"

#include "meshtinker/gltf/document.hpp"
#include "meshtinker/gltf/geometry.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>

namespace fs = std::filesystem;
using namespace meshtinker;
using gltf::Document;
using gltf::Json;

namespace {

struct Run {
    compress::Report report;
    common::Diagnostics diagnostics;
    fs::path model;
};

Run run(const fs::path& input, const std::string& name) {
    const fs::path directory = test::workDirectory("compress/" + name);
    Run result;
    compress::Options options;
    common::Workspace workspace(directory, false);
    common::Outputs outputs;
    const std::string stem = input.stem().u8string();
    result.report = compress::compress(input, directory, stem, options, workspace, outputs, result.diagnostics);
    outputs.commit();
    result.model = directory / (stem + ".gltf");
    return result;
}

Run run(const char* fixture) { return run(test::fixture(fixture).model, fixture); }

struct Corner {
    std::array<double, 3> position{};
    // NORMAL in world space and normalized, then the other attributes, by name.
    std::vector<double> attributes;
};

// What a primitive draws: its corners in world space, through `world`.
std::vector<Corner> corners(Document& document, common::Workspace& workspace, std::size_t m, std::size_t p,
                            const std::array<float, 16>& world) {
    gltf::GeometryReader reader(document, workspace);
    reader.preparePrimitive(m, p);
    const cgltf_primitive& primitive = document.data().meshes[m].primitives[p];
    std::map<std::string, const cgltf_accessor*> attributes;
    for (std::size_t a = 0; a < primitive.attributes_count; ++a) {
        reader.prepare(*primitive.attributes[a].data);
        attributes[primitive.attributes[a].name] = primitive.attributes[a].data;
    }
    if (primitive.indices != nullptr) {
        reader.prepare(*primitive.indices);
    }
    const auto transform = [&](const float* v, float w) {
        std::array<double, 3> out{};
        for (std::size_t i = 0; i < 3; ++i) {
            out[i] = world[i] * v[0] + world[4 + i] * v[1] + world[8 + i] * v[2] + world[12 + i] * w;
        }
        return out;
    };
    const std::size_t count = gltf::cornerCount(primitive);
    std::vector<Corner> result;
    for (std::size_t c = 0; c < count; ++c) {
        const std::size_t v = primitive.indices != nullptr ? cgltf_accessor_read_index(primitive.indices, c) : c;
        Corner corner;
        float value[4] = {};
        cgltf_accessor_read_float(attributes.at("POSITION"), v, value, 3);
        corner.position = transform(value, 1);
        for (const auto& [name, accessor] : attributes) {
            if (name == "POSITION") {
                continue;
            }
            const std::size_t n = cgltf_num_components(accessor->type);
            cgltf_accessor_read_float(accessor, v, value, n);
            if (name == "NORMAL") {
                // Fixtures have no shear: the matrix's linear part, normalized after, is enough.
                std::array<double, 3> normal = transform(value, 0);
                const double length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
                for (double& x : normal) {
                    x /= length > 0 ? length : 1;
                }
                corner.attributes.insert(corner.attributes.end(), normal.begin(), normal.end());
            } else {
                corner.attributes.insert(corner.attributes.end(), value, value + n);
            }
        }
        result.push_back(std::move(corner));
    }
    return result;
}

std::array<float, 16> worldOf(const Document& document, std::size_t mesh) {
    std::array<float, 16> world{};
    for (std::size_t n = 0; n < document.data().nodes_count; ++n) {
        const cgltf_node& node = document.data().nodes[n];
        if (node.mesh == document.data().meshes + mesh) {
            cgltf_node_transform_world(&node, world.data());
            return world;
        }
    }
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

struct Tolerance {
    double position;
    double attribute;
};

bool close(const Corner& a, const Corner& b, const Tolerance& t) {
    for (std::size_t k = 0; k < 3; ++k) {
        if (std::abs(a.position[k] - b.position[k]) > t.position) {
            return false;
        }
    }
    if (a.attributes.size() != b.attributes.size()) {
        return false;
    }
    for (std::size_t k = 0; k < a.attributes.size(); ++k) {
        if (std::abs(a.attributes[k] - b.attributes[k]) > t.attribute) {
            return false;
        }
    }
    return true;
}

// Strips and fans as the lists they draw, with their winding.
std::vector<Corner> asList(const std::vector<Corner>& corners, cgltf_primitive_type type) {
    if (type == cgltf_primitive_type_triangles) {
        return corners;
    }
    std::vector<Corner> list;
    for (std::size_t t = 0; t + 2 < corners.size(); ++t) {
        const std::array<std::size_t, 3> c = type == cgltf_primitive_type_triangle_fan ? std::array<std::size_t, 3>{0, t + 1, t + 2}
                                             : t % 2 == 1                               ? std::array<std::size_t, 3>{t + 1, t, t + 2}
                                                                                        : std::array<std::size_t, 3>{t, t + 1, t + 2};
        for (const std::size_t k : c) {
            list.push_back(corners[k]);
        }
    }
    return list;
}

// Every triangle of `b` matches a different triangle of `a`, winding kept.
std::size_t unmatchedTriangles(const std::vector<Corner>& a, const std::vector<Corner>& b, const Tolerance& t) {
    const std::size_t count = a.size() / 3;
    if (b.size() / 3 != count) {
        return std::max(count, b.size() / 3);
    }
    std::vector<bool> used(count, false);
    std::size_t unmatched = 0;
    for (std::size_t j = 0; j < count; ++j) {
        bool found = false;
        for (std::size_t i = 0; i < count && !found; ++i) {
            if (used[i]) {
                continue;
            }
            for (std::size_t r = 0; r < 3 && !found; ++r) {
                if (close(a[3 * i + r], b[3 * j], t) && close(a[3 * i + (r + 1) % 3], b[3 * j + 1], t) &&
                    close(a[3 * i + (r + 2) % 3], b[3 * j + 2], t)) {
                    used[i] = true;
                    found = true;
                }
            }
        }
        unmatched += found ? 0 : 1;
    }
    return unmatched;
}

// The output draws what the input draws, within the quantization's error:
// 14-bit positions on the mesh's longest side, 8-bit octahedral normals,
// 12-bit texture coordinates, 8-bit colors and weights.
void checkDecodesClose(const char* fixture) {
    const Run result = run(fixture);
    Document input = Document::read(test::fixture(fixture).model);
    Document output = Document::read(result.model);
    common::Workspace workspace(test::workDirectory(std::string("compress/") + fixture + "-read"), false);
    CHECK_EQ(output.data().meshes_count, input.data().meshes_count);
    // A grid spans the meshes of one space: the model's extent bounds it.
    std::vector<std::vector<std::vector<Corner>>> drawn(input.data().meshes_count);
    std::array<double, 3> low{1e300, 1e300, 1e300};
    std::array<double, 3> high{-1e300, -1e300, -1e300};
    for (std::size_t m = 0; m < input.data().meshes_count; ++m) {
        for (std::size_t p = 0; p < input.data().meshes[m].primitives_count; ++p) {
            drawn[m].push_back(corners(input, workspace, m, p, worldOf(input, m)));
            for (const Corner& c : drawn[m].back()) {
                for (std::size_t k = 0; k < 3; ++k) {
                    low[k] = std::min(low[k], c.position[k]);
                    high[k] = std::max(high[k], c.position[k]);
                }
            }
        }
    }
    const double extent = std::max({high[0] - low[0], high[1] - low[1], high[2] - low[2]});
    // Half a step of 2^13 − 1 over half the extent, with float noise.
    const Tolerance tolerance{extent / 2 / 8191 * 0.5 * 1.01 + 1e-6, 0.025};
    for (std::size_t m = 0; m < input.data().meshes_count && m < output.data().meshes_count; ++m) {
        for (std::size_t p = 0; p < input.data().meshes[m].primitives_count; ++p) {
            const cgltf_primitive& source = input.data().meshes[m].primitives[p];
            const cgltf_primitive& written = output.data().meshes[m].primitives[p];
            const std::vector<Corner>& a = drawn[m][p];
            std::vector<Corner> b = corners(output, workspace, m, p, worldOf(output, m));
            const bool triangles = source.type == cgltf_primitive_type_triangles ||
                                   source.type == cgltf_primitive_type_triangle_strip ||
                                   source.type == cgltf_primitive_type_triangle_fan;
            if (triangles) {
                CHECK_EQ(unmatchedTriangles(asList(a, source.type), asList(b, written.type), tolerance), std::size_t{0});
            } else {
                CHECK_EQ(a.size(), b.size());
                for (std::size_t c = 0; c < a.size() && c < b.size(); ++c) {
                    CHECK(close(a[c], b[c], tolerance));
                }
            }
        }
    }
    if (test::failureCount() > 0) {
        std::fprintf(stderr, "  in fixture %s\n", fixture);
    }
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

bool listed(const Json& json, const char* list, const char* name) {
    const auto found = json.find(list);
    return found != json.end() && std::find(found->begin(), found->end(), Json(name)) != found->end();
}

} // namespace

TEST_CASE(decodes_close_to_the_input) {
    for (const char* fixture : {"triangle", "scan", "scan-tiles", "scan-meshes", "shared", "modes", "interleaved", "sparse", "morph",
                                "skinned", "instancing", "quantized", "rigid-hierarchy", "occupancy"}) {
        checkDecodesClose(fixture);
    }
}

TEST_CASE(compression_is_required) {
    const Run result = run("scan-tiles");
    CHECK_EQ(result.report.compressed, std::uint64_t{4});
    CHECK(result.report.output_bytes < result.report.input_bytes / 4);
    const Document output = Document::read(result.model);
    const Json& json = output.json();
    CHECK(listed(json, "extensionsRequired", "EXT_meshopt_compression"));
    CHECK(listed(json, "extensionsRequired", "KHR_mesh_quantization"));
    const Json& fallback = json["buffers"].back();
    CHECK(fallback["extensions"]["EXT_meshopt_compression"]["fallback"].get<bool>());
    CHECK(!fallback.contains("uri"));
}

// The grid's dequantization goes into the mesh's node when nothing else
// depends on that node's transform, and into a new child otherwise.
TEST_CASE(grid_goes_to_the_node_or_a_child) {
    const Run result = run("rigid-hierarchy");
    CHECK_EQ(result.report.quantized_meshes, std::uint64_t{3});
    CHECK_EQ(result.report.added_nodes, std::uint64_t{2});
    const Document output = Document::read(result.model);
    const Json& nodes = output.json()["nodes"];
    CHECK_EQ(nodes.size(), std::size_t{5});
    for (const Json& node : nodes) {
        const std::string name = node.value("name", "");
        if (name == "Parent" || name == "Animated") {
            CHECK(!node.contains("mesh"));
            CHECK(!node.contains("scale"));
        }
        if (name == "Plain") {
            CHECK(node.contains("mesh"));
        }
    }
}

// Meshes drawn in one space share one grid, so that the positions on their
// shared borders quantize to the same values on both sides.
TEST_CASE(meshes_in_one_space_share_a_grid) {
    const Run result = run("scan-meshes");
    CHECK_EQ(result.report.quantized_meshes, std::uint64_t{4});
    const Document output = Document::read(result.model);
    const Json& nodes = output.json()["nodes"];
    CHECK_EQ(nodes.size(), std::size_t{4});
    for (const Json& node : nodes) {
        CHECK(node["scale"] == nodes[0]["scale"]);
        CHECK(node["translation"] == nodes[0]["translation"]);
    }
}

// Skinned meshes ignore their node's transform, instancing applies before
// it, and morph targets would need the grid too: their positions stay float.
TEST_CASE(positions_stay_float_where_the_node_cant_take_the_grid) {
    for (const char* fixture : {"skinned", "instancing", "morph"}) {
        const Run result = run(fixture);
        CHECK_EQ(result.report.float_meshes, std::uint64_t{1});
        CHECK_EQ(result.report.quantized_meshes, std::uint64_t{0});
        const Document output = Document::read(result.model);
        const cgltf_accessor* position =
            gltf::findAttribute(output.data().meshes[0].primitives[0], cgltf_attribute_type_position, 0);
        CHECK(position->component_type == cgltf_component_type_r_32f);
    }
}

TEST_CASE(draco_and_meshopt_are_kept) {
    const Run draco = run("draco");
    CHECK_EQ(draco.report.kept_draco, std::uint64_t{1});
    CHECK_EQ(draco.diagnostics.toJson()[0]["code"].get<std::string>(), std::string("draco-kept"));
    CHECK(Document::read(draco.model).data().meshes[0].primitives[0].has_draco_mesh_compression);
    const Run meshopt = run("meshopt");
    CHECK_EQ(meshopt.report.kept_meshopt, std::uint64_t{1});
    CHECK_EQ(meshopt.report.compressed, std::uint64_t{0});
}

// A compressed model has nothing left to compress: a second run writes the same files.
TEST_CASE(compressing_twice_changes_nothing) {
    const Run once = run("scan-tiles");
    const Run twice = run(once.model, "scan-tiles-twice");
    CHECK_EQ(twice.report.kept_meshopt, std::uint64_t{4});
    CHECK(filesIn(once.model.parent_path()) == filesIn(twice.model.parent_path()));
}

TEST_CASE(accessor_extras_are_kept) {
    const Run result = run("unknown-ext");
    const Document output = Document::read(result.model);
    const Json& json = output.json();
    bool found = false;
    for (const Json& accessor : json["accessors"]) {
        found = found || (accessor.contains("extras") && accessor["extras"].value("from", "") == "accessor");
    }
    CHECK(found);
}

// Rounding doesn't change a vertex's total weight, even when the input's isn't one.
TEST_CASE(weight_sums_are_kept) {
    const Run result = run("invalid");
    Document output = Document::read(result.model);
    common::Workspace workspace(test::workDirectory("compress/invalid-read"), false);
    gltf::GeometryReader reader(output, workspace);
    const cgltf_accessor* weights =
        gltf::findAttribute(output.data().meshes[0].primitives[0], cgltf_attribute_type_weights, 0);
    CHECK(reader.prepare(*weights) == gltf::Readability::readable);
    for (std::size_t v = 0; v < weights->count; ++v) {
        float w[4] = {};
        cgltf_accessor_read_float(weights, v, w, 4);
        CHECK(std::abs(w[0] + w[1] + w[2] + w[3] - 0.8F) < 1.0F / 255);
    }
}
