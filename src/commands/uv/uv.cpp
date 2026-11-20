#include "uv.hpp"

#include "bake.hpp"

#include "meshtinker/gltf/edit.hpp"
#include "meshtinker/gltf/geometry.hpp"
#include "meshtinker/gltf/graph.hpp"
#include "meshtinker/gltf/layout.hpp"
#include "meshtinker/gltf/material_slots.hpp"
#include "meshtinker/gltf/primitive_data.hpp"
#include "meshtinker/gltf/selection.hpp"
#include "meshtinker/image/image.hpp"

#include <xatlas.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <set>

namespace meshtinker::uv {

namespace fs = std::filesystem;
using Json = gltf::Json;

const char* statusName(Status status) {
    switch (status) {
    case Status::repacked:
        return "repacked";
    case Status::no_gain:
        return "no-gain";
    case Status::skipped:
        return "skipped";
    }
    return "";
}

namespace {

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }

constexpr int repeat = 10497;
constexpr int mirrored_repeat = 33648;

std::size_t find(std::vector<std::size_t>& parent, std::size_t i) {
    while (parent[i] != i) {
        parent[i] = parent[parent[i]];
        i = parent[i];
    }
    return i;
}

// One texture a group's materials sample.
struct TextureUse {
    std::size_t texture = 0;
    std::optional<std::size_t> image;
    int texcoord = 0;
    bool transform = false;
    bool repeats = false;
    image::Content content = image::Content::linear;
};

struct Primitive {
    std::size_t mesh = 0;
    std::size_t primitive = 0;
    gltf::TriangleData data;
    // TEXCOORD_<set> among data.attributes.
    std::size_t texcoord = 0;
    // The first vertex in the group's UV mesh.
    std::uint32_t offset = 0;
};

struct XatlasDeleter {
    void operator()(xatlas::Atlas* atlas) const { xatlas::Destroy(atlas); }
};

// Whether a primitive's geometry is Draco- or meshopt-compressed.
bool compressed_geometry(const cgltf_primitive& primitive) {
    if (primitive.has_draco_mesh_compression != 0) {
        return true;
    }
    const auto meshopt = [](const cgltf_accessor* accessor) {
        return accessor != nullptr && accessor->buffer_view != nullptr && accessor->buffer_view->has_meshopt_compression != 0;
    };
    if (meshopt(primitive.indices)) {
        return true;
    }
    for (std::size_t a = 0; a < primitive.attributes_count; ++a) {
        if (meshopt(primitive.attributes[a].data)) {
            return true;
        }
    }
    for (std::size_t t = 0; t < primitive.targets_count; ++t) {
        for (std::size_t a = 0; a < primitive.targets[t].attributes_count; ++a) {
            if (meshopt(primitive.targets[t].attributes[a].data)) {
                return true;
            }
        }
    }
    return false;
}

std::string materialName(const Json& json, std::size_t material) {
    const Json& m = json["materials"][material];
    if (m.contains("name") && m["name"].is_string()) {
        return m["name"].get<std::string>();
    }
    return "material " + std::to_string(material);
}

image::Content contentOf(const std::string& path) {
    const std::optional<gltf::KnownSlot> known = gltf::knownSlot(path);
    if (!known) {
        return image::Content::linear;
    }
    switch (known->kind) {
    case gltf::SlotKind::color:
        return image::Content::srgb;
    case gltf::SlotKind::normal:
        return image::Content::normal;
    case gltf::SlotKind::data:
        return image::Content::linear;
    }
    return image::Content::linear;
}

} // namespace

Report uv(const fs::path& input, const fs::path& directory, const std::string& stem, const Options& options,
          common::Workspace& workspace, common::Outputs& outputs, common::Diagnostics& diagnostics) {
    const Clock::time_point start = Clock::now();
    Report report;
    gltf::Document document = gltf::readNormalized(input, options.read, diagnostics);
    gltf::Layout layout = gltf::normalizedLayout(document, gltf::selectScene(document), diagnostics);
    gltf::GeometryReader reader(document, workspace);
    Json& json = layout.json;
    const cgltf_data& data = document.data();
    const std::vector<std::string> source_names = gltf::imageFileNames(document, stem);
    gltf::ImageNames names(document, stem);
    const std::size_t material_count = json.contains("materials") ? json["materials"].size() : 0;

    // What each material samples, and the groups of materials that share images.
    std::vector<std::vector<TextureUse>> uses(material_count);
    std::vector<std::size_t> parent(material_count);
    std::iota(parent.begin(), parent.end(), std::size_t{0});
    std::map<std::size_t, std::size_t> image_owner;
    for (std::size_t m = 0; m < material_count; ++m) {
        for (const gltf::TextureSlot& slot : gltf::textureSlots(json["materials"][m])) {
            const std::size_t texture = (*slot.info)["index"].get<std::size_t>();
            if (!json.contains("textures") || texture >= json["textures"].size()) {
                continue;
            }
            const Json& texture_json = json["textures"][texture];
            TextureUse use;
            use.texture = texture;
            use.texcoord = slot.info->value("texCoord", 0);
            use.transform = slot.info->contains("extensions") && (*slot.info)["extensions"].contains("KHR_texture_transform");
            use.content = contentOf(slot.path);
            if (texture_json.contains("source") && texture_json["source"].is_number_unsigned() &&
                !(texture_json.contains("extensions") && texture_json["extensions"].contains("KHR_texture_basisu"))) {
                use.image = texture_json["source"].get<std::size_t>();
            }
            int wrap_s = repeat;
            int wrap_t = repeat;
            if (texture_json.contains("sampler") && json.contains("samplers")) {
                const Json& sampler = json["samplers"][texture_json["sampler"].get<std::size_t>()];
                wrap_s = sampler.value("wrapS", repeat);
                wrap_t = sampler.value("wrapT", repeat);
            }
            use.repeats = wrap_s == repeat || wrap_s == mirrored_repeat || wrap_t == repeat || wrap_t == mirrored_repeat;
            if (use.image) {
                const auto [owner, inserted] = image_owner.emplace(*use.image, m);
                if (!inserted) {
                    parent[find(parent, m)] = find(parent, owner->second);
                }
            }
            uses[m].push_back(use);
        }
    }
    std::map<std::size_t, std::vector<std::size_t>> groups;
    for (std::size_t m = 0; m < material_count; ++m) {
        if (!uses[m].empty()) {
            groups[find(parent, m)].push_back(m);
        }
    }
    std::map<std::size_t, std::vector<std::pair<std::size_t, std::size_t>>> primitives_of;
    for (std::size_t mesh = 0; mesh < data.meshes_count; ++mesh) {
        for (std::size_t p = 0; p < data.meshes[mesh].primitives_count; ++p) {
            const cgltf_material* material = data.meshes[mesh].primitives[p].material;
            if (material != nullptr) {
                primitives_of[static_cast<std::size_t>(material - data.materials)].emplace_back(mesh, p);
            }
        }
    }

    // Rewritten primitives, whose arrays the written views read.
    std::deque<Primitive> written;
    for (const auto& [root, materials] : groups) {
        const Clock::time_point group_start = Clock::now();
        Group group;
        for (const std::size_t m : materials) {
            group.materials.push_back(materialName(json, m));
        }
        const auto skip = [&](const char* code, const std::string& message) {
            group.status = Status::skipped;
            group.reason = code;
            diagnostics.warn(code, message, group.materials.front());
        };

        // The images, the texture coordinate set, and what is left as it is.
        std::set<std::size_t> images;
        std::set<int> sets;
        std::map<std::size_t, image::Content> content;
        bool transform = false;
        bool repeats = false;
        bool undecodable = false;
        bool missing = false;
        bool compressed = false;
        for (const std::size_t m : materials) {
            for (const TextureUse& use : uses[m]) {
                sets.insert(use.texcoord);
                transform = transform || use.transform;
                repeats = repeats || use.repeats;
                if (!use.image) {
                    const Json& texture = json["textures"][use.texture];
                    compressed = compressed || (texture.contains("extensions") && texture["extensions"].contains("KHR_texture_basisu"));
                    undecodable = true;
                    continue;
                }
                images.insert(*use.image);
                auto [entry, inserted] = content.emplace(*use.image, use.content);
                if (!inserted && use.content == image::Content::normal) {
                    entry->second = image::Content::normal;
                }
            }
        }
        for (const std::size_t i : images) {
            const gltf::ImageData& image = document.image(layout.image_sources[i]);
            if (image.missing) {
                missing = true;
            } else if (image.header && image.header->format == gltf::ImageFormat::ktx2) {
                compressed = true;
            } else if (!image.header ||
                       (image.header->format != gltf::ImageFormat::png && image.header->format != gltf::ImageFormat::jpeg)) {
                undecodable = true;
            }
            group.images.push_back({source_names[layout.image_sources[i]], image.header ? image.header->width : 0,
                                    image.header ? image.header->height : 0, {}, 0, 0});
        }
        const auto finish = [&] {
            group.seconds = secondsSince(group_start);
            report.groups.push_back(std::move(group));
        };
        if (compressed) {
            skip("image-already-compressed", "materials with KTX2 images are left as they are: run uv before ktx");
            finish();
            continue;
        }
        if (missing) {
            skip("image-missing", "materials whose image files are missing are left as they are");
            finish();
            continue;
        }
        if (undecodable) {
            skip("image-format-unsupported", "materials with images other than PNG and JPEG are left as they are");
            finish();
            continue;
        }
        if (sets.size() != 1) {
            skip("uv-mixed-texcoords", "materials whose textures use different texture coordinate sets are left as they are");
            finish();
            continue;
        }
        if (transform) {
            skip("uv-texture-transform", "materials with KHR_texture_transform are left as they are");
            finish();
            continue;
        }
        const int set = *sets.begin();
        const std::string texcoord_name = "TEXCOORD_" + std::to_string(set);

        // Small images whose texels are all within 4 levels of each other read
        // the same at any UV: they keep their file and stay out of the layout.
        std::vector<bool> uniform(group.images.size(), false);
        {
            constexpr std::uint64_t small = std::uint64_t{256} * 256;
            std::size_t k = 0;
            for (const std::size_t i : images) {
                const GroupImage& image = group.images[k];
                if (static_cast<std::uint64_t>(image.width) * image.height <= small) {
                    std::string error;
                    if (const std::optional<image::Image> decoded =
                            image::decode(document.image(layout.image_sources[i]).bytes, error)) {
                        std::array<int, 4> low = {255, 255, 255, 255};
                        std::array<int, 4> high = {0, 0, 0, 0};
                        for (std::size_t t = 0; t < decoded->pixels.size(); ++t) {
                            low[t % 4] = std::min<int>(low[t % 4], decoded->pixels[t]);
                            high[t % 4] = std::max<int>(high[t % 4], decoded->pixels[t]);
                        }
                        constexpr int noise = 4; // JPEG noise on a flat image: 0 to 3 on the local scans
                        uniform[k] = high[0] - low[0] <= noise && high[1] - low[1] <= noise &&
                                     high[2] - low[2] <= noise && high[3] - low[3] <= noise;
                    }
                }
                ++k;
            }
        }
        // The layout is in texels of the largest image ("fine" texels). Every
        // other image is a whole fraction of it in both directions, at most 4
        // times smaller, and moves are whole multiples of every fraction, so
        // that each image moves by whole texels of its own.
        std::uint32_t fine_width = 0;
        std::uint32_t fine_height = 0;
        for (std::size_t k = 0; k < group.images.size(); ++k) {
            const GroupImage& image = group.images[k];
            if (!uniform[k] && static_cast<std::uint64_t>(image.width) * image.height >
                                   static_cast<std::uint64_t>(fine_width) * fine_height) {
                fine_width = image.width;
                fine_height = image.height;
            }
        }
        if (fine_width == 0) {
            group.status = Status::no_gain; // Only uniform images.
            finish();
            continue;
        }
        std::vector<std::uint32_t> divisors(group.images.size(), 1);
        std::uint32_t snap = 1;
        bool sizes_ok = true;
        for (std::size_t k = 0; k < group.images.size(); ++k) {
            const GroupImage& image = group.images[k];
            if (uniform[k]) {
                continue;
            }
            const std::uint32_t divisor = image.width == 0 ? 0 : fine_width / image.width;
            sizes_ok = sizes_ok && divisor != 0 && image.width * divisor == fine_width && image.height * divisor == fine_height;
            divisors[k] = divisor;
            snap = divisor == 0 ? snap : std::lcm(snap, divisor);
        }
        if (!sizes_ok || snap > 4) {
            skip("uv-image-sizes",
                 "materials whose images aren't whole fractions of the largest, or more than 4 times smaller, are left as "
                 "they are");
            finish();
            continue;
        }

        // The group's primitives, with their texture coordinates.
        std::vector<Primitive> primitives;
        bool readable = true;
        bool morphed = false;
        bool outside = false;
        std::uint64_t vertex_total = 0;
        for (const std::size_t m : materials) {
            for (const auto& [mesh, p] : primitives_of[m]) {
                Primitive primitive;
                primitive.mesh = mesh;
                primitive.primitive = p;
                if (gltf::readTriangles(document, reader, workspace, mesh, p, primitive.data) != gltf::TriangleReadStatus::ok) {
                    readable = false;
                    break;
                }
                const auto& attributes = primitive.data.attributes;
                const auto found = std::find_if(attributes.begin(), attributes.end(),
                                                [&](const gltf::AttributeData& a) { return a.name == texcoord_name; });
                if (found == attributes.end()) {
                    readable = false;
                    break;
                }
                primitive.texcoord = static_cast<std::size_t>(found - attributes.begin());
                for (const auto& target : primitive.data.targets) {
                    for (const gltf::AttributeData& a : target) {
                        morphed = morphed || a.name == texcoord_name;
                    }
                }
                const auto& uvs = found->values;
                for (std::size_t i = 0; i < uvs.size(); ++i) {
                    outside = outside || uvs[i] < -1e-4f || uvs[i] > 1.0f + 1e-4f;
                }
                primitive.offset = static_cast<std::uint32_t>(vertex_total);
                vertex_total += primitive.data.vertex_count;
                primitives.push_back(std::move(primitive));
            }
            if (!readable) {
                break;
            }
        }
        group.primitives = primitives.size();
        if (!readable || vertex_total > UINT32_MAX) {
            skip("uv-unreadable-geometry",
                 "materials drawn by points, lines, or primitives without their texture coordinates are left as they are");
            finish();
            continue;
        }
        if (morphed) {
            skip("uv-morphed-texcoords", "materials whose morph targets move texture coordinates are left as they are");
            finish();
            continue;
        }
        if (outside && repeats) {
            skip("uv-texture-repeats", "materials that tile a repeating texture are left as they are");
            finish();
            continue;
        }

        // The UV mesh, in base texels.
        std::vector<float> uv_mesh(vertex_total * 2);
        std::vector<std::uint32_t> indices;
        for (const Primitive& primitive : primitives) {
            const auto& uvs = primitive.data.attributes[primitive.texcoord].values;
            for (std::size_t v = 0; v < primitive.data.vertex_count; ++v) {
                uv_mesh[(primitive.offset + v) * 2] = uvs[v * 2] * static_cast<float>(fine_width);
                uv_mesh[(primitive.offset + v) * 2 + 1] = uvs[v * 2 + 1] * static_cast<float>(fine_height);
            }
            for (const std::uint32_t index : primitive.data.indices) {
                indices.push_back(primitive.offset + index);
            }
        }
        std::unique_ptr<xatlas::Atlas, XatlasDeleter> atlas(xatlas::Create());
        xatlas::UvMeshDecl decl;
        decl.vertexUvData = uv_mesh.data();
        decl.vertexCount = static_cast<std::uint32_t>(vertex_total);
        decl.vertexStride = sizeof(float) * 2;
        decl.indexData = indices.data();
        decl.indexCount = static_cast<std::uint32_t>(indices.size());
        decl.indexFormat = xatlas::IndexFormat::UInt32;
        if (xatlas::AddUvMesh(atlas.get(), decl) != xatlas::AddMeshError::Success) {
            skip("uv-pack-failed", "materials xatlas can't pack are left as they are");
            finish();
            continue;
        }
        xatlas::ComputeCharts(atlas.get());
        xatlas::PackOptions pack;
        // In texels of the smallest image, as the rounding of moves is.
        pack.padding = options.padding * snap;
        pack.texelsPerUnit = 1.0f;
        pack.bilinear = true;
        // Block alignment would stretch charts by up to 4 texels.
        pack.blockAlign = false;
        pack.bruteForce = options.brute_force;
        // Moves only: rotating a chart would turn its normal map's tangent
        // space, and anything but whole texels would resample.
        pack.rotateCharts = false;
        pack.rotateChartsToAxis = false;
        xatlas::PackCharts(atlas.get(), pack);
        group.charts = atlas->chartCount;

        // Each chart's move, rounded to whole base texels. xatlas stretches
        // every chart until its extents are whole texels (PackCharts, "Scale
        // the charts to use the entire texel area available"), by less than a
        // texel without block alignment. The chart is moved unstretched, by
        // the move of its minimum corner: it stays within 1.5 texels of the
        // room xatlas gave it, which the padding absorbs.
        const xatlas::Mesh& packed = atlas->meshes[0];
        std::vector<std::int32_t> chart_of(vertex_total, -1);
        constexpr double far = 1e30;
        std::vector<std::array<double, 4>> corners(atlas->chartCount, {far, far, far, far});
        for (std::uint32_t i = 0; i < packed.vertexCount; ++i) {
            const xatlas::Vertex& vertex = packed.vertexArray[i];
            if (vertex.chartIndex < 0 || vertex.xref >= vertex_total) {
                continue;
            }
            auto& c = corners[static_cast<std::size_t>(vertex.chartIndex)];
            const std::size_t xref = vertex.xref;
            c[0] = std::min(c[0], static_cast<double>(uv_mesh[xref * 2]));
            c[1] = std::min(c[1], static_cast<double>(uv_mesh[xref * 2 + 1]));
            c[2] = std::min(c[2], static_cast<double>(vertex.uv[0]));
            c[3] = std::min(c[3], static_cast<double>(vertex.uv[1]));
            chart_of[vertex.xref] = vertex.chartIndex;
        }
        std::vector<std::array<std::int64_t, 2>> moves(atlas->chartCount);
        for (std::size_t c = 0; c < moves.size(); ++c) {
            const auto step = static_cast<double>(snap);
            moves[c] = {std::llround((corners[c][2] - corners[c][0]) / step) * snap,
                        std::llround((corners[c][3] - corners[c][1]) / step) * snap};
        }
        // Vertices of triangles xatlas left out (zero area in UV space) move
        // with a chart that shares their UV, else with any chart of their
        // triangle.
        std::uint64_t unplaced = 0;
        {
            std::map<std::pair<float, float>, std::int32_t> chart_at;
            for (std::size_t v = 0; v < vertex_total; ++v) {
                if (chart_of[v] >= 0) {
                    chart_at.emplace(std::make_pair(uv_mesh[v * 2], uv_mesh[v * 2 + 1]), chart_of[v]);
                }
            }
            for (std::size_t v = 0; v < vertex_total; ++v) {
                if (chart_of[v] < 0) {
                    const auto found = chart_at.find(std::make_pair(uv_mesh[v * 2], uv_mesh[v * 2 + 1]));
                    if (found != chart_at.end()) {
                        chart_of[v] = found->second;
                    }
                }
            }
            for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
                std::int32_t chart = -1;
                for (std::size_t k = 0; k < 3; ++k) {
                    chart = std::max(chart, chart_of[indices[t + k]]);
                }
                for (std::size_t k = 0; k < 3 && chart >= 0; ++k) {
                    if (chart_of[indices[t + k]] < 0) {
                        chart_of[indices[t + k]] = chart;
                    }
                }
            }
            // Triangles still out of every chart have no area in UV space and
            // share no UV with a chart: each samples one point or line of the
            // source. They move with the chart of the placed vertex nearest
            // their centroid, which keeps those texels when the point is in
            // or near that chart. A brute-force search, so only for a few.
            constexpr std::size_t max_isolated = 1000;
            std::vector<std::size_t> isolated;
            for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
                if (chart_of[indices[t]] < 0 && chart_of[indices[t + 1]] < 0 && chart_of[indices[t + 2]] < 0) {
                    isolated.push_back(t);
                }
            }
            if (!isolated.empty() && isolated.size() <= max_isolated) {
                for (const std::size_t t : isolated) {
                    double cx = 0;
                    double cy = 0;
                    for (std::size_t k = 0; k < 3; ++k) {
                        cx += static_cast<double>(uv_mesh[static_cast<std::size_t>(indices[t + k]) * 2]) / 3;
                        cy += static_cast<double>(uv_mesh[static_cast<std::size_t>(indices[t + k]) * 2 + 1]) / 3;
                    }
                    double best = 1e300;
                    std::int32_t chart = -1;
                    for (std::size_t v = 0; v < vertex_total; ++v) {
                        if (chart_of[v] < 0) {
                            continue;
                        }
                        const double dx = static_cast<double>(uv_mesh[v * 2]) - cx;
                        const double dy = static_cast<double>(uv_mesh[v * 2 + 1]) - cy;
                        if (dx * dx + dy * dy < best) {
                            best = dx * dx + dy * dy;
                            chart = chart_of[v];
                        }
                    }
                    for (std::size_t k = 0; k < 3; ++k) {
                        chart_of[indices[t + k]] = chart;
                    }
                }
            }
            // Vertices no triangle uses aren't drawn: they keep their UVs.
            std::vector<bool> used(vertex_total, false);
            for (const std::uint32_t index : indices) {
                used[index] = true;
            }
            for (std::size_t v = 0; v < vertex_total; ++v) {
                unplaced += chart_of[v] < 0 && used[v] ? 1u : 0u;
            }
        }
        if (unplaced > 0) {
            skip("uv-unplaced-vertices", "materials with vertices in no chart are left as they are");
            finish();
            continue;
        }

        // The atlas's size: the moved charts' bounds with a margin of 2 base
        // texels on every side, so that the texels copied around a chart at
        // the edge (1.5 texels, bake.hpp) stay inside, in multiples of 4 for
        // ktx. xatlas places some charts against the edge, without padding.
        const double margin = 2.0 * snap;
        double min_x = 1e30;
        double min_y = 1e30;
        double max_x = -1e30;
        double max_y = -1e30;
        for (std::size_t v = 0; v < vertex_total; ++v) {
            if (chart_of[v] < 0) {
                continue; // Not drawn.
            }
            const auto& m = moves[static_cast<std::size_t>(chart_of[v])];
            const double x = static_cast<double>(uv_mesh[v * 2]) + static_cast<double>(m[0]);
            const double y = static_cast<double>(uv_mesh[v * 2 + 1]) + static_cast<double>(m[1]);
            min_x = std::min(min_x, x);
            min_y = std::min(min_y, y);
            max_x = std::max(max_x, x);
            max_y = std::max(max_y, y);
        }
        const auto shift_x = static_cast<std::int64_t>(std::ceil((margin - min_x) / snap)) * snap;
        const auto shift_y = static_cast<std::int64_t>(std::ceil((margin - min_y) / snap)) * snap;
        for (auto& m : moves) {
            m[0] += shift_x;
            m[1] += shift_y;
        }
        const image::Size atlas_size = image::fittedSize(
            {static_cast<std::uint32_t>(std::ceil(max_x + static_cast<double>(shift_x) + margin)),
             static_cast<std::uint32_t>(std::ceil(max_y + static_cast<double>(shift_y) + margin))},
            0, 4 * snap);
        for (std::size_t k = 0; k < group.images.size(); ++k) {
            group.texels_before += static_cast<std::uint64_t>(group.images[k].width) * group.images[k].height;
            group.texels_after += uniform[k] ? static_cast<std::uint64_t>(group.images[k].width) * group.images[k].height
                                             : static_cast<std::uint64_t>(atlas_size.width / divisors[k]) *
                                                   (atlas_size.height / divisors[k]);
        }
        if (static_cast<double>(group.texels_after) > (1.0 - options.min_gain) * static_cast<double>(group.texels_before)) {
            // texels_after keeps the atlas's size, for measurements.
            group.status = Status::no_gain;
            finish();
            continue;
        }

        // Bake each image into the new layout.
        std::vector<BakeTriangle> triangles;
        triangles.reserve(indices.size() / 3);
        for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
            const std::uint32_t a = indices[t];
            const auto& m = moves[static_cast<std::size_t>(chart_of[a])];
            BakeTriangle triangle;
            bool same = true;
            for (std::size_t k = 0; k < 3; ++k) {
                const std::size_t v = indices[t + k];
                triangle.x[k] = uv_mesh[v * 2];
                triangle.y[k] = uv_mesh[v * 2 + 1];
                same = same && chart_of[indices[t + k]] == chart_of[a];
            }
            if (!same) {
                continue; // Zero area in UV space, across charts: it covers no texel.
            }
            triangle.dx = m[0];
            triangle.dy = m[1];
            triangles.push_back(triangle);
        }
        std::uint64_t covered = 0;
        std::uint64_t texels_written = 0;
        std::map<std::size_t, std::size_t> new_image;
        std::size_t k = 0;
        for (const std::size_t i : images) {
            const std::size_t source = layout.image_sources[i];
            if (uniform[k]) {
                texels_written += static_cast<std::uint64_t>(group.images[k].width) * group.images[k].height;
                ++k;
                continue;
            }
            std::string error;
            std::optional<image::Image> decoded = image::decode(document.image(source).bytes, error);
            if (!decoded) {
                break;
            }
            const std::uint32_t divisor = divisors[k];
            Baked baked =
                bake(*decoded, divisor, {atlas_size.width / divisor, atlas_size.height / divisor}, triangles);
            decoded.reset();
            covered += baked.covered;
            const image::Size fitted = image::fittedSize({baked.image.width, baked.image.height}, options.max_size, 1);
            if (fitted != image::Size{baked.image.width, baked.image.height}) {
                const bool alpha = image::analyze(baked.image).alpha;
                baked.image = image::resize(baked.image, fitted, content[i], alpha);
            }
            texels_written += baked.image.texels();
            const int channels = baked.image.source_channels == 2 || baked.image.source_channels == 4 ? 4 : 3;
            const std::vector<std::uint8_t> png = image::encodePng(baked.image, channels);
            const std::string name = names.reserve(fs::u8path(source_names[source]).replace_extension(".png").u8string());
            common::OutputFile file = outputs.create(directory / fs::u8path(name));
            file.write({png.data(), png.size()});
            file.close();
            Json image_json = json["images"][i];
            image_json.erase("uri");
            image_json.erase("bufferView");
            image_json["mimeType"] = "image/png";
            new_image[i] = gltf::addImage(layout, std::move(image_json), name, png.size());
            group.images[k].output = name;
            group.images[k].output_width = baked.image.width;
            group.images[k].output_height = baked.image.height;
            ++k;
            document.releaseMappedPages();
        }
        if (k != images.size()) {
            skip("image-decode-failed", "materials whose images can't be decoded are left as they are");
            finish();
            continue;
        }
        group.texels_after = texels_written;
        group.occupancy_before = static_cast<double>(covered) / static_cast<double>(group.texels_before);
        group.occupancy_after = static_cast<double>(covered) / static_cast<double>(group.texels_after);
        for (const std::size_t m : materials) {
            for (const TextureUse& use : uses[m]) {
                // Uniform images keep their file.
                if (const auto found = new_image.find(*use.image); found != new_image.end()) {
                    json["textures"][use.texture]["source"] = found->second;
                }
            }
        }

        // New texture coordinates: the moved charts, over the atlas's size.
        for (Primitive& primitive : primitives) {
            auto& uvs = primitive.data.attributes[primitive.texcoord].values;
            for (std::size_t v = 0; v < primitive.data.vertex_count; ++v) {
                const std::size_t g = primitive.offset + v;
                if (chart_of[g] < 0) {
                    continue; // Not drawn: its UV stays.
                }
                const auto& m = moves[static_cast<std::size_t>(chart_of[g])];
                uvs[v * 2] = static_cast<float>((static_cast<double>(uv_mesh[g * 2]) + static_cast<double>(m[0])) /
                                                atlas_size.width);
                uvs[v * 2 + 1] = static_cast<float>(
                    (static_cast<double>(uv_mesh[g * 2 + 1]) + static_cast<double>(m[1])) / atlas_size.height);
            }
            written.push_back(std::move(primitive));
            Primitive& kept = written.back();
            const cgltf_primitive& source = data.meshes[kept.mesh].primitives[kept.primitive];
            if (compressed_geometry(source)) {
                // Draco or meshopt: the whole primitive is written plain.
                gltf::replaceGeometry(layout, kept.mesh, kept.primitive, gltf::addPlainGeometry(layout, kept.data));
            } else {
                // Only the texture coordinates change: the other accessors stay.
                const Json& old = json["meshes"][kept.mesh]["primitives"][kept.primitive];
                gltf::PrimitiveGeometry geometry;
                geometry.attributes = old["attributes"];
                geometry.attributes[texcoord_name] =
                    gltf::addPlainAttribute(layout, kept.data.attributes[kept.texcoord], false);
                if (old.contains("indices")) {
                    geometry.indices = old["indices"].get<std::size_t>();
                }
                geometry.targets = old.value("targets", Json());
                geometry.mode = old.value("mode", 4);
                gltf::replaceGeometry(layout, kept.mesh, kept.primitive, geometry);
            }
        }
        group.status = Status::repacked;
        finish();
    }

    gltf::prune(layout);
    report.files = gltf::writeSeparate(document, layout, directory, stem, outputs);
    report.seconds = secondsSince(start);
    return report;
}

} // namespace meshtinker::uv
