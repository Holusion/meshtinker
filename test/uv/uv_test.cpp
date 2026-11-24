#include "commands/uv/uv.hpp"

#include "meshtinker/common/diagnostics.hpp"
#include "meshtinker/common/outputs.hpp"
#include "meshtinker/common/workspace.hpp"
#include "meshtinker/gltf/geometry.hpp"
#include "meshtinker/gltf/layout.hpp"
#include "meshtinker/gltf/primitive_data.hpp"
#include "meshtinker/image/image.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace meshtinker;
using Json = nlohmann::ordered_json;

namespace {

struct Run {
    uv::Report report;
    common::Diagnostics diagnostics;
    fs::path model;
};

Run repack(const char* fixture, const char* name, const uv::Options& options = {}) {
    Run run;
    const fs::path directory = test::workDirectory(std::string("uv/") + name);
    common::Workspace workspace(directory, false);
    common::Outputs outputs;
    uv::Options trusted = options;
    trusted.read.trusted = true;
    run.report = uv::uv(test::fixture(fixture).model, directory, name, trusted, workspace, outputs, run.diagnostics);
    outputs.commit();
    run.model = directory / (std::string(name) + ".gltf");
    return run;
}

// Bilinear sampling with texel centers at half-integers, clamped at the edges.
std::array<double, 4> sample(const image::Image& image, double u, double v) {
    const double x = u * image.width - 0.5;
    const double y = v * image.height - 0.5;
    const double fx = x - std::floor(x);
    const double fy = y - std::floor(y);
    const auto x0 = static_cast<std::int64_t>(std::floor(x));
    const auto y0 = static_cast<std::int64_t>(std::floor(y));
    std::array<double, 4> out = {0, 0, 0, 0};
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            const auto tx = std::clamp<std::int64_t>(x0 + i, 0, image.width - 1);
            const auto ty = std::clamp<std::int64_t>(y0 + j, 0, image.height - 1);
            const double w = (i == 0 ? 1 - fx : fx) * (j == 0 ? 1 - fy : fy);
            const std::size_t t = static_cast<std::size_t>(ty * image.width + tx) * 4;
            for (std::size_t c = 0; c < 4; ++c) {
                out[c] += w * image.pixels[t + c];
            }
        }
    }
    return out;
}

struct Model {
    gltf::Document document;
    std::vector<std::vector<gltf::TriangleData>> primitives;
};

image::Image imageOf(const gltf::Document& document, std::size_t texture) {
    const Json& texture_json = document.json()["textures"][texture];
    const std::size_t source = texture_json.contains("source")
                                   ? texture_json["source"].get<std::size_t>()
                                   : texture_json["extensions"]["KHR_texture_basisu"]["source"].get<std::size_t>();
    std::string error;
    std::optional<image::Image> decoded = image::decode(document.image(source).bytes, error);
    return decoded ? *decoded : image::Image{};
}

// The largest difference, over sample points inside every triangle, between
// each texture of the input at the old UVs and of the output at the new ones.
double largestDifference(const fs::path& input, const fs::path& output, const fs::path& scratch) {
    common::Diagnostics diagnostics;
    common::Workspace workspace(scratch, false);
    gltf::Document before = gltf::readNormalized(input, {true}, diagnostics);
    gltf::Document after = gltf::readNormalized(output, {true}, diagnostics);
    gltf::GeometryReader before_reader(before, workspace);
    gltf::GeometryReader after_reader(after, workspace);
    const cgltf_data& data = before.data();
    double largest = 0;
    std::uint32_t seed = 12345;
    const auto random = [&] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<double>(seed >> 8) / static_cast<double>(1u << 24);
    };
    for (std::size_t m = 0; m < data.meshes_count; ++m) {
        for (std::size_t p = 0; p < data.meshes[m].primitives_count; ++p) {
            const cgltf_primitive& primitive = data.meshes[m].primitives[p];
            if (primitive.material == nullptr) {
                continue;
            }
            const auto material = static_cast<std::size_t>(primitive.material - data.materials);
            gltf::TriangleData a;
            gltf::TriangleData b;
            if (gltf::readTriangles(before, before_reader, workspace, m, p, a) != gltf::TriangleReadStatus::ok ||
                gltf::readTriangles(after, after_reader, workspace, m, p, b) != gltf::TriangleReadStatus::ok) {
                continue;
            }
            const auto texcoords = [](const gltf::TriangleData& d) -> const gltf::AttributeData* {
                for (const gltf::AttributeData& attribute : d.attributes) {
                    if (attribute.name == "TEXCOORD_0") {
                        return &attribute;
                    }
                }
                return nullptr;
            };
            const gltf::AttributeData* old_uv = texcoords(a);
            const gltf::AttributeData* new_uv = texcoords(b);
            CHECK(old_uv != nullptr && new_uv != nullptr && a.indices.size() == b.indices.size());
            if (old_uv == nullptr || new_uv == nullptr || a.indices.size() != b.indices.size()) {
                continue;
            }
            std::vector<std::size_t> textures;
            const Json& material_json = before.json()["materials"][material];
            if (material_json.contains("pbrMetallicRoughness") && material_json["pbrMetallicRoughness"].contains("baseColorTexture")) {
                textures.push_back(material_json["pbrMetallicRoughness"]["baseColorTexture"]["index"].get<std::size_t>());
            }
            if (material_json.contains("normalTexture")) {
                textures.push_back(material_json["normalTexture"]["index"].get<std::size_t>());
            }
            for (const std::size_t texture : textures) {
                const image::Image old_image = imageOf(before, texture);
                const image::Image new_image = imageOf(after, texture);
                for (std::size_t t = 0; t + 2 < a.indices.size(); t += 3) {
                    for (int k = 0; k < 8; ++k) {
                        double w0 = random();
                        double w1 = random();
                        if (w0 + w1 > 1) {
                            w0 = 1 - w0;
                            w1 = 1 - w1;
                        }
                        const double w[3] = {w0, w1, 1 - w0 - w1};
                        double ou = 0;
                        double ov = 0;
                        double nu = 0;
                        double nv = 0;
                        for (std::size_t c = 0; c < 3; ++c) {
                            const std::size_t va = a.indices[t + c];
                            const std::size_t vb = b.indices[t + c];
                            ou += w[c] * old_uv->values[va * 2];
                            ov += w[c] * old_uv->values[va * 2 + 1];
                            nu += w[c] * new_uv->values[vb * 2];
                            nv += w[c] * new_uv->values[vb * 2 + 1];
                        }
                        const std::array<double, 4> x = sample(old_image, ou, ov);
                        const std::array<double, 4> y = sample(new_image, nu, nv);
                        for (std::size_t c = 0; c < 4; ++c) {
                            largest = std::max(largest, std::abs(x[c] - y[c]));
                        }
                    }
                }
            }
        }
    }
    return largest;
}

const uv::Group* groupWith(const uv::Report& report, const std::string& material) {
    for (const uv::Group& group : report.groups) {
        for (const std::string& name : group.materials) {
            if (name == material) {
                return &group;
            }
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE(repacked_textures_read_the_same) {
    for (const char* fixture : {"scan-tiles", "scan-meshes", "atlas", "occupancy", "udim"}) {
        const Run run = repack(fixture, fixture);
        std::uint64_t repacked = 0;
        for (const uv::Group& group : run.report.groups) {
            repacked += group.status == uv::Status::repacked ? 1 : 0;
        }
        CHECK(repacked > 0);
        // Whole-texel moves: the same texels under the same weights.
        const double difference =
            largestDifference(test::fixture(fixture).model, run.model, test::workDirectory(std::string("uv/scratch-") + fixture));
        if (difference > 0.01) {
            test::fail(std::string(fixture) + ": sampled difference " + std::to_string(difference), __FILE__, __LINE__);
        }
    }
}

TEST_CASE(images_keep_their_ratio_and_groups_share_a_layout) {
    const Run run = repack("atlas", "atlas-groups");
    CHECK_EQ(run.report.groups.size(), std::size_t{1});
    const uv::Group& group = run.report.groups.front();
    CHECK(group.status == uv::Status::repacked);
    CHECK_EQ(group.materials.size(), std::size_t{2});
    CHECK_EQ(group.images.size(), std::size_t{2});
    CHECK(group.images[0].output_width == 2 * group.images[1].output_width);
    CHECK(group.images[0].output_height == 2 * group.images[1].output_height);
    CHECK(group.images[0].output_width % 4 == 0 && group.images[1].output_width % 4 == 0);
    CHECK(group.texels_after < group.texels_before);
    CHECK(group.occupancy_after > group.occupancy_before);
    const Json json = Json::parse(std::ifstream(run.model));
    for (const Json& image : json["images"]) {
        CHECK(image["uri"] == "atlas-color_1.png" || image["uri"] == "atlas-normal_1.png");
    }
}

TEST_CASE(what_is_left_as_it_is) {
    const Run occupancy = repack("occupancy", "occupancy-skips");
    const std::map<std::string, std::string> expected = {{"Transform", "uv-texture-transform"},
                                                         {"Repeat", "uv-texture-repeats"},
                                                         {"TwoSlots", "uv-mixed-texcoords"}};
    for (const auto& [material, code] : expected) {
        const uv::Group* group = groupWith(occupancy.report, material);
        CHECK(group != nullptr && group->status == uv::Status::skipped && group->reason == code);
    }
    // Every image of AllSlots covers its whole quad: nothing to gain.
    const uv::Group* full = groupWith(occupancy.report, "AllSlots");
    CHECK(full != nullptr && full->status == uv::Status::no_gain);

    const Run alt = repack("alt-sources", "alt-sources");
    CHECK(groupWith(alt.report, "KtxOnly") != nullptr && groupWith(alt.report, "KtxOnly")->reason == "image-already-compressed");
    CHECK(groupWith(alt.report, "WebpOnly") != nullptr && groupWith(alt.report, "WebpOnly")->reason == "image-format-unsupported");
}

// KTX2 images are left as they are, unless --max-size shrinks them: then
// they are baked into PNG, and the textures lose KHR_texture_basisu.
TEST_CASE(ktx2_is_baked_only_when_shrunk) {
    const Run kept = repack("ktx2-sources", "ktx2-kept");
    const uv::Group* group = groupWith(kept.report, "Etc1s");
    CHECK(group != nullptr && group->reason == "image-already-compressed");

    // 48 is under the images' 64, and over the repacked pages: they are baked,
    // and not resampled after.
    uv::Options options;
    options.max_size = 48;
    const Run run = repack("ktx2-sources", "ktx2-shrunk", options);
    group = groupWith(run.report, "Etc1s");
    CHECK(group != nullptr && group->status == uv::Status::repacked);
    CHECK(groupWith(run.report, "Uastc") == group);
    if (group != nullptr) {
        for (const uv::GroupImage& image : group->images) {
            CHECK(image.output_width <= 48 && image.output_height <= 48);
        }
    }
    // The fully used alpha image has nothing to gain, and stays KTX2.
    const Json json = Json::parse(std::ifstream(run.model));
    std::size_t basisu = 0;
    for (const Json& texture : json["textures"]) {
        if (texture.contains("extensions") && texture["extensions"].contains("KHR_texture_basisu")) {
            ++basisu;
        }
    }
    CHECK_EQ(basisu, std::size_t{1});
    CHECK(json.contains("extensionsUsed"));
    // Old textures at old UVs read close to new textures at new UVs: the
    // ETC1S and UASTC blocks are baked as decoded.
    const double difference =
        largestDifference(test::fixture("ktx2-sources").model, run.model, test::workDirectory("uv/scratch-ktx2"));
    CHECK(difference < 0.01);
}

TEST_CASE(max_size_scales_the_atlas_down) {
    uv::Options options;
    options.max_size = 16;
    const Run run = repack("atlas", "atlas-max", options);
    const uv::Group& group = run.report.groups.front();
    CHECK(group.status == uv::Status::repacked);
    CHECK(group.images[0].output_width <= 16 && group.images[0].output_height <= 16);
    // Occupancy after is the baked layout's, which resizing doesn't change.
    const Run full = repack("atlas", "atlas-full");
    CHECK(std::abs(group.occupancy_after - full.report.groups.front().occupancy_after) < 1e-9);
    CHECK(group.occupancy_after <= 1);
}
