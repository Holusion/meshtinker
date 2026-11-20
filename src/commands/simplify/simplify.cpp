#include "simplify.hpp"

#include "meshtinker/common/large_array.hpp"
#include "meshtinker/gltf/edit.hpp"
#include "meshtinker/gltf/geometry.hpp"
#include "meshtinker/gltf/layout.hpp"
#include "meshtinker/gltf/meshopt_memory.hpp"
#include "meshtinker/gltf/primitive_data.hpp"

#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <limits>
#include <optional>

namespace meshtinker::simplify {
namespace {

// A vertex position as its exact bits, with −0 read as 0.
using PositionKey = std::array<std::uint32_t, 3>;

PositionKey keyOf(const float* p) {
    PositionKey key{};
    for (std::size_t k = 0; k < 3; ++k) {
        const float value = p[k] == 0.0F ? 0.0F : p[k];
        std::memcpy(&key[k], &value, sizeof(float));
    }
    return key;
}

struct KeyedVertex {
    PositionKey key;
    std::uint32_t primitive;
};

// The longest side of the bounding box of every primitive's positions, as
// meshopt_simplifyScale measures one primitive.
float meshScale(const std::vector<std::optional<gltf::TriangleData>>& primitives) {
    std::array<float, 3> min{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::max()};
    std::array<float, 3> max{-min[0], -min[1], -min[2]};
    for (const auto& data : primitives) {
        if (!data) {
            continue;
        }
        const common::LargeArray<float>& positions = data->position().values;
        for (std::size_t v = 0; v < data->vertex_count; ++v) {
            for (std::size_t k = 0; k < 3; ++k) {
                min[k] = std::min(min[k], positions[3 * v + k]);
                max[k] = std::max(max[k], positions[3 * v + k]);
            }
        }
    }
    float scale = 0;
    for (std::size_t k = 0; k < 3; ++k) {
        scale = std::max(scale, max[k] - min[k]);
    }
    return scale;
}

// Positions that the triangles of two or more primitives of a mesh share,
// sorted. Locking them keeps the borders between primitives sealed: each
// primitive is simplified on its own, and would otherwise collapse a shared
// border its own way.
common::LargeArray<PositionKey> sharedPositions(const std::vector<std::optional<gltf::TriangleData>>& primitives,
                                                common::Workspace& workspace) {
    std::size_t total = 0;
    for (const auto& data : primitives) {
        total += data ? data->vertex_count : 0;
    }
    common::LargeArray<KeyedVertex> keyed(workspace, total);
    std::size_t count = 0;
    for (std::size_t p = 0; p < primitives.size(); ++p) {
        const std::optional<gltf::TriangleData>& entry = primitives[p];
        if (!entry) {
            continue;
        }
        const gltf::TriangleData& data = *entry;
        // Only vertices the primitive's triangles use: primitives that share
        // one vertex accessor share only the vertices both draw.
        common::LargeArray<std::uint8_t> used(workspace, data.vertex_count);
        for (const std::uint32_t index : data.indices) {
            used[index] = 1;
        }
        const common::LargeArray<float>& positions = data.position().values;
        for (std::size_t v = 0; v < data.vertex_count; ++v) {
            if (used[v] != 0) {
                keyed[count++] = {keyOf(&positions[3 * v]), static_cast<std::uint32_t>(p)};
            }
        }
    }
    std::sort(keyed.begin(), keyed.begin() + count, [](const KeyedVertex& a, const KeyedVertex& b) {
        return a.key != b.key ? a.key < b.key : a.primitive < b.primitive;
    });
    std::size_t shared_count = 0;
    for (std::size_t i = 0; i < count;) {
        std::size_t j = i + 1;
        bool shared = false;
        while (j < count && keyed[j].key == keyed[i].key) {
            shared = shared || keyed[j].primitive != keyed[i].primitive;
            ++j;
        }
        if (shared) {
            // Written over entries already read.
            keyed[shared_count++].key = keyed[i].key;
        }
        i = j;
    }
    common::LargeArray<PositionKey> result(workspace, shared_count);
    for (std::size_t i = 0; i < shared_count; ++i) {
        result[i] = keyed[i].key;
    }
    return result;
}

// Keeps the vertices `indices` use, in the order they first appear, and
// renumbers `indices` to match (meshopt_optimizeVertexFetchRemap).
gltf::TriangleData compact(const gltf::TriangleData& source, common::LargeArray<std::uint32_t>&& indices,
                           common::Workspace& workspace) {
    common::LargeArray<std::uint32_t> remap(workspace, source.vertex_count);
    const std::size_t unique =
        meshopt_optimizeVertexFetchRemap(remap.data(), indices.data(), indices.size(), source.vertex_count);
    meshopt_remapIndexBuffer(indices.data(), indices.data(), indices.size(), remap.data());
    gltf::TriangleData result;
    result.vertex_count = unique;
    result.indices = std::move(indices);
    const auto remapAttribute = [&](const gltf::AttributeData& attribute) {
        gltf::AttributeData out;
        out.name = attribute.name;
        out.components = attribute.components;
        out.source_component_type = attribute.source_component_type;
        out.values = common::LargeArray<float>(workspace, unique * attribute.components);
        meshopt_remapVertexBuffer(out.values.data(), attribute.values.data(), source.vertex_count,
                                  attribute.components * sizeof(float), remap.data());
        return out;
    };
    result.attributes.reserve(source.attributes.size());
    for (const gltf::AttributeData& attribute : source.attributes) {
        result.attributes.push_back(remapAttribute(attribute));
    }
    for (const std::vector<gltf::AttributeData>& target : source.targets) {
        std::vector<gltf::AttributeData> out;
        out.reserve(target.size());
        for (const gltf::AttributeData& attribute : target) {
            out.push_back(remapAttribute(attribute));
        }
        result.targets.push_back(std::move(out));
    }
    return result;
}

std::string primitiveName(std::size_t mesh, std::size_t primitive) {
    return "mesh " + std::to_string(mesh) + " primitive " + std::to_string(primitive);
}

} // namespace

Report simplify(const std::filesystem::path& input, const std::filesystem::path& directory, const std::string& stem,
                const Options& options, common::Workspace& workspace, common::Outputs& outputs,
                common::Diagnostics& diagnostics) {
    gltf::Document document = gltf::readNormalized(input, options.read, diagnostics);
    gltf::Layout layout = gltf::normalizedLayout(document, gltf::selectScene(document), diagnostics);
    gltf::GeometryReader reader(document, workspace);
    const gltf::MeshoptMemory meshopt_memory(workspace);

    Report report;
    report.target_error = options.error;
    // Simplified primitives, whose arrays the written views read.
    std::deque<gltf::TriangleData> written;
    const cgltf_data& data = document.data();
    for (std::size_t m = 0; m < data.meshes_count; ++m) {
        const cgltf_mesh& mesh = data.meshes[m];
        std::vector<std::optional<gltf::TriangleData>> primitives(mesh.primitives_count);
        for (std::size_t p = 0; p < mesh.primitives_count; ++p) {
            gltf::TriangleData triangles;
            switch (gltf::readTriangles(document, reader, workspace, m, p, triangles)) {
            case gltf::TriangleReadStatus::ok:
                primitives[p] = std::move(triangles);
                break;
            case gltf::TriangleReadStatus::not_triangles:
                ++report.skipped;
                diagnostics.warn("points-and-lines-kept", "Points and lines were copied without simplification",
                                 primitiveName(m, p));
                break;
            case gltf::TriangleReadStatus::unsupported:
            case gltf::TriangleReadStatus::unreadable:
                ++report.skipped;
                diagnostics.warn("primitive-not-simplified",
                                 "Primitives whose geometry can't be read were copied as they are",
                                 primitiveName(m, p));
                break;
            }
        }
        const float scale = meshScale(primitives);
        const float error = static_cast<float>(options.error) * scale;
        std::size_t read = 0;
        for (const auto& primitive : primitives) {
            read += primitive ? 1U : 0U;
        }
        const common::LargeArray<PositionKey> shared =
            read > 1 ? sharedPositions(primitives, workspace) : common::LargeArray<PositionKey>();

        for (std::size_t p = 0; p < primitives.size(); ++p) {
            const std::optional<gltf::TriangleData>& entry = primitives[p];
            if (!entry) {
                continue;
            }
            const gltf::TriangleData& source = *entry;
            const common::LargeArray<float>& positions = source.position().values;
            ++report.input.primitives;
            report.input.vertices += source.vertex_count;
            report.input.faces += source.indices.size() / 3;

            common::LargeArray<std::uint8_t> lock;
            if (!shared.empty()) {
                lock = common::LargeArray<std::uint8_t>(workspace, source.vertex_count);
                for (std::size_t v = 0; v < source.vertex_count; ++v) {
                    lock[v] = std::binary_search(shared.begin(), shared.end(), keyOf(&positions[3 * v])) ? 1 : 0;
                }
            }
            common::LargeArray<std::uint32_t> indices(workspace, source.indices.size());
            float result_error = 0;
            // No target count: collapse until the error bound, best effort,
            // with nothing pruned (glTF-Transform's simplify as eCorpus runs
            // it, but with the error measured on the whole mesh).
            const std::size_t count = meshopt_simplifyWithAttributes(
                indices.data(), source.indices.data(), source.indices.size(), positions.data(), source.vertex_count,
                3 * sizeof(float), nullptr, 0, nullptr, 0, lock.empty() ? nullptr : lock.data(), 0, error,
                meshopt_SimplifyErrorAbsolute, &result_error);
            if (count == source.indices.size() || count == 0 || scale <= 0) {
                ++report.unchanged;
                ++report.output.primitives;
                report.output.vertices += source.vertex_count;
                report.output.faces += source.indices.size() / 3;
                continue;
            }
            indices.shrink(count);
            written.push_back(compact(source, std::move(indices), workspace));
            gltf::TriangleData& result = written.back();
            ++report.simplified;
            ++report.output.primitives;
            report.output.vertices += result.vertex_count;
            report.output.faces += result.indices.size() / 3;
            report.achieved_error = std::max(report.achieved_error, static_cast<double>(result_error / scale));
            gltf::replaceGeometry(layout, m, p, gltf::addPlainGeometry(layout, result));
        }
    }
    gltf::prune(layout);
    report.files = gltf::writeSeparate(document, layout, directory, stem, outputs);
    return report;
}

} // namespace meshtinker::simplify
