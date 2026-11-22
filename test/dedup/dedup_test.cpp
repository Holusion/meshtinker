#include "commands/dedup/dedup.hpp"

#include "meshtinker/gltf/document.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <cgltf.h>

#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace meshtinker;

namespace {

struct Run {
    dedup::Report report;
    common::Diagnostics diagnostics;
    fs::path directory;
    fs::path model;
};

Run run(const fs::path& model, const std::string& name, bool dry_run = false) {
    Run result;
    result.directory = test::workDirectory("dedup/" + name);
    common::Outputs outputs;
    dedup::Options options;
    options.dry_run = dry_run;
    result.report = dedup::dedup(model, result.directory, name, options, outputs, result.diagnostics);
    outputs.commit();
    result.model = result.directory / (name + ".gltf");
    return result;
}

void checkCount(const dedup::Count& count, const nlohmann::json& expected) {
    CHECK_EQ(count.before, expected["before"].get<std::uint64_t>());
    CHECK_EQ(count.after, expected["after"].get<std::uint64_t>());
}

std::string floats(const cgltf_accessor* accessor) {
    std::string out;
    if (accessor == nullptr) {
        return out;
    }
    const std::size_t n = cgltf_num_components(accessor->type);
    std::vector<float> values(n);
    for (std::size_t i = 0; i < accessor->count; ++i) {
        cgltf_accessor_read_float(accessor, i, values.data(), n);
        for (const float value : values) {
            out += std::to_string(value) + ',';
        }
    }
    return out;
}

// What a node draws, by node name: its mesh's triangles with every attribute,
// and the texture its material shows (image bytes, sampler filters), with
// the material's extras. Names of meshes and materials are left out: those
// of merged objects go.
std::map<std::string, std::string> drawn(const fs::path& model) {
    const gltf::Document document = gltf::Document::read(model);
    const cgltf_data& data = document.data();
    std::map<std::string, std::string> result;
    for (std::size_t n = 0; n < data.nodes_count; ++n) {
        const cgltf_node& node = data.nodes[n];
        std::string& out = result[node.name != nullptr ? node.name : std::to_string(n)];
        if (node.mesh == nullptr) {
            continue;
        }
        for (std::size_t p = 0; p < node.mesh->primitives_count; ++p) {
            const cgltf_primitive& primitive = node.mesh->primitives[p];
            for (std::size_t a = 0; a < primitive.attributes_count; ++a) {
                out += std::string(primitive.attributes[a].name) + ':' + floats(primitive.attributes[a].data) + ';';
            }
            out += "indices:" + floats(primitive.indices) + ';';
            const cgltf_material* material = primitive.material;
            if (material == nullptr) {
                continue;
            }
            const cgltf_texture* texture = material->pbr_metallic_roughness.base_color_texture.texture;
            if (texture != nullptr && texture->image != nullptr) {
                const gltf::ImageData& image = document.image(static_cast<std::size_t>(texture->image - data.images));
                out += "image:" + std::string(reinterpret_cast<const char*>(image.bytes.data), image.bytes.size) + ';';
            }
            if (texture != nullptr && texture->sampler != nullptr) {
                out += "sampler:" + std::to_string(texture->sampler->mag_filter) + '/' +
                       std::to_string(texture->sampler->min_filter) + ';';
            }
            const nlohmann::ordered_json& json = document.json()["materials"][static_cast<std::size_t>(material - data.materials)];
            out += "extras:" + json.value("extras", nlohmann::ordered_json()).dump() + ';';
        }
    }
    return result;
}

} // namespace

// Each kind merges what expected.json says. Bytes are those stored: the
// image that names wall.png a second time was stored once already, and the
// interleaved view goes once neither of its accessors is used.
TEST_CASE(duplicates_merge_into_the_first) {
    const test::Fixture& fixture = test::fixture("duplicates");
    const Run result = run(fixture.model, "counts");
    const nlohmann::json& expected = fixture.expected["dedup"];
    checkCount(result.report.images, expected["images"]);
    checkCount(result.report.samplers, expected["samplers"]);
    checkCount(result.report.textures, expected["textures"]);
    checkCount(result.report.materials, expected["materials"]);
    checkCount(result.report.accessors, expected["accessors"]);
    checkCount(result.report.meshes, expected["meshes"]);
    checkCount(result.report.view_bytes, expected["bytes"]["bufferViews"]);
    checkCount(result.report.image_bytes, expected["bytes"]["images"]);
    CHECK(result.diagnostics.toJson().empty());
    for (const auto& name : expected["absentFiles"]) {
        CHECK(!fs::exists(result.directory / name.get<std::string>()));
    }
    CHECK(fs::exists(result.directory / "wall.png"));
    CHECK(fs::exists(result.directory / "other.png"));
}

// Every node draws what it drew, with the same image, sampler and extras;
// merged nodes share the first one's mesh, which keeps its name.
TEST_CASE(nodes_draw_what_they_drew) {
    const test::Fixture& fixture = test::fixture("duplicates");
    const Run result = run(fixture.model, "drawn");
    CHECK(drawn(result.model) == drawn(fixture.model));

    const gltf::Document output = gltf::Document::read(result.model);
    const cgltf_data& data = output.data();
    std::map<std::string, const cgltf_mesh*> meshes;
    std::vector<std::string> names;
    for (std::size_t n = 0; n < data.nodes_count; ++n) {
        meshes[data.nodes[n].name] = data.nodes[n].mesh;
    }
    for (const auto& [node, first] : fixture.expected["dedup"]["sameMesh"].items()) {
        CHECK(meshes[node] != nullptr && meshes[node] == meshes[first.get<std::string>()]);
    }
    names.reserve(data.meshes_count + data.materials_count);
    for (std::size_t m = 0; m < data.meshes_count; ++m) {
        names.emplace_back(data.meshes[m].name != nullptr ? data.meshes[m].name : "");
    }
    for (std::size_t m = 0; m < data.materials_count; ++m) {
        names.emplace_back(data.materials[m].name != nullptr ? data.materials[m].name : "");
    }
    for (const auto& name : fixture.expected["dedup"]["keptNames"]) {
        CHECK(std::find(names.begin(), names.end(), name.get<std::string>()) != names.end());
    }
    // The animation's two samplers read one input.
    CHECK(data.animations_count == 1 && data.animations[0].samplers_count == 2);
    CHECK(data.animations[0].samplers[0].input == data.animations[0].samplers[1].input);
}

// A second run finds nothing more to merge, and draws the same.
TEST_CASE(second_run_merges_nothing) {
    const test::Fixture& fixture = test::fixture("duplicates");
    const Run first = run(fixture.model, "first");
    const Run second = run(first.model, "second");
    const dedup::Count* counts[] = {&second.report.images,    &second.report.samplers,  &second.report.textures,
                                    &second.report.materials, &second.report.accessors, &second.report.meshes};
    for (const dedup::Count* count : counts) {
        CHECK_EQ(count->before, count->after);
    }
    CHECK_EQ(second.report.view_bytes.before, second.report.view_bytes.after);
    CHECK_EQ(second.report.image_bytes.before, second.report.image_bytes.after);
    CHECK(drawn(second.model) == drawn(fixture.model));
}

// Other fixtures: the shared mesh and accessor stay shared, distinct objects
// stay apart, and what each node draws doesn't change.
TEST_CASE(other_fixtures_draw_the_same) {
    for (const char* name : {"shared", "textures", "interleaved", "skinned", "scan-tiles", "instancing", "morph"}) {
        const test::Fixture& fixture = test::fixture(name);
        const Run result = run(fixture.model, std::string("fixture-") + name);
        CHECK(drawn(result.model) == drawn(fixture.model));
    }
}

// --dry-run counts the same and writes nothing.
TEST_CASE(dry_run_writes_nothing) {
    const test::Fixture& fixture = test::fixture("duplicates");
    const Run result = run(fixture.model, "dry-run", true);
    CHECK_EQ(result.report.meshes.after, fixture.expected["dedup"]["meshes"]["after"].get<std::uint64_t>());
    CHECK(result.report.files.empty());
    CHECK(fs::is_empty(result.directory));
}
