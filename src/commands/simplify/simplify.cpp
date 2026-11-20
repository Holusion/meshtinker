#include "simplify.hpp"

#include "meshtinker/common/large_array.hpp"
#include "meshtinker/gltf/edit.hpp"
#include "meshtinker/gltf/geometry.hpp"
#include "meshtinker/gltf/layout.hpp"
#include "meshtinker/gltf/meshopt_memory.hpp"
#include "meshtinker/gltf/primitive_data.hpp"
#include "meshtinker/gltf/transform.hpp"

#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>
#include <functional>
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

// The scene's size, and each mesh's scale on its way there.
struct SceneScale {
    // The longest side of the scene's bounding box, in world space.
    double extent = 0;
    // By mesh: the largest axis scale of any node (or instance) that draws it,
    // 0 for a mesh no node draws.
    std::vector<double> mesh_scale;
};

double axisScale(const gltf::Matrix& m) {
    double scale = 0;
    for (std::size_t c = 0; c < 3; ++c) {
        scale = std::max(scale, std::sqrt(m[4 * c] * m[4 * c] + m[4 * c + 1] * m[4 * c + 1] + m[4 * c + 2] * m[4 * c + 2]));
    }
    return scale;
}

// Measures the scene drawn from the primitives read, through every node that
// draws their mesh. An instanced node contributes the corners of its mesh's
// bounding box for each instance, rather than every vertex; skinned meshes
// are placed by their node, as `inspect` places them.
SceneScale sceneScale(gltf::Document& document, gltf::GeometryReader& reader,
                      const std::vector<std::pair<std::size_t, std::size_t>>& members,
                      const std::vector<std::optional<gltf::TriangleData>>& primitives) {
    const cgltf_data& data = document.data();
    SceneScale result;
    result.mesh_scale.assign(data.meshes_count, 0);
    std::vector<std::array<double, 6>> local(data.meshes_count, {1e300, 1e300, 1e300, -1e300, -1e300, -1e300});
    for (std::size_t i = 0; i < members.size(); ++i) {
        const std::optional<gltf::TriangleData>& entry = primitives[i];
        if (!entry) {
            continue;
        }
        std::array<double, 6>& box = local[members[i].first];
        const common::LargeArray<float>& positions = entry->position().values;
        for (std::size_t v = 0; v + 2 < positions.size(); v += 3) {
            for (std::size_t k = 0; k < 3; ++k) {
                box[k] = std::min(box[k], static_cast<double>(positions[v + k]));
                box[3 + k] = std::max(box[3 + k], static_cast<double>(positions[v + k]));
            }
        }
    }
    std::array<double, 3> min{1e300, 1e300, 1e300};
    std::array<double, 3> max{-1e300, -1e300, -1e300};
    const auto extend = [&](const std::array<double, 3>& p) {
        for (std::size_t k = 0; k < 3; ++k) {
            min[k] = std::min(min[k], p[k]);
            max[k] = std::max(max[k], p[k]);
        }
    };
    for (std::size_t n = 0; n < data.nodes_count; ++n) {
        const cgltf_node& node = data.nodes[n];
        if (node.mesh == nullptr) {
            continue;
        }
        const auto m = static_cast<std::size_t>(node.mesh - data.meshes);
        const std::array<double, 6>& box = local[m];
        if (box[0] > box[3]) {
            continue;
        }
        const gltf::Matrix world = gltf::worldMatrix(node);
        const gltf::Instancing instances = gltf::instancing(node);
        for (const cgltf_accessor* accessor : {instances.translation, instances.rotation, instances.scale}) {
            if (accessor != nullptr) {
                reader.prepare(*accessor);
            }
        }
        for (std::size_t i = 0; i < instances.count; ++i) {
            const gltf::Matrix matrix =
                node.has_mesh_gpu_instancing != 0 ? gltf::multiply(world, gltf::instanceMatrix(instances, i)) : world;
            result.mesh_scale[m] = std::max(result.mesh_scale[m], axisScale(matrix));
            if (node.has_mesh_gpu_instancing != 0) {
                for (std::size_t corner = 0; corner < 8; ++corner) {
                    extend(gltf::apply(matrix, {box[(corner & 1) != 0 ? 3 : 0], box[(corner & 2) != 0 ? 4 : 1],
                                                box[(corner & 4) != 0 ? 5 : 2]}));
                }
                continue;
            }
            for (std::size_t j = 0; j < members.size(); ++j) {
                const std::optional<gltf::TriangleData>& entry = primitives[j];
                if (!entry || members[j].first != m) {
                    continue;
                }
                const common::LargeArray<float>& positions = entry->position().values;
                for (std::size_t v = 0; v + 2 < positions.size(); v += 3) {
                    extend(gltf::apply(matrix, {positions[v], positions[v + 1], positions[v + 2]}));
                }
            }
        }
    }
    for (std::size_t k = 0; k < 3; ++k) {
        result.extent = std::max(result.extent, max[k] - min[k]);
    }
    return result;
}

// Positions that the triangles of two or more primitives of the scene share,
// sorted, compared as stored: tiles written as primitives of one mesh, or as
// meshes drawn by nodes with the same transform, share their borders' positions. Locking them keeps the borders between primitives sealed: each
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
    // The whole scene is read at once: primitives that share a border are
    // locked along it, wherever they are, and the error is measured on the
    // scene's size.
    std::vector<std::pair<std::size_t, std::size_t>> members;
    std::vector<std::optional<gltf::TriangleData>> primitives;
    for (std::size_t m = 0; m < data.meshes_count; ++m) {
        for (std::size_t p = 0; p < data.meshes[m].primitives_count; ++p) {
            gltf::TriangleData triangles;
            members.emplace_back(m, p);
            primitives.emplace_back();
            switch (gltf::readTriangles(document, reader, workspace, m, p, triangles)) {
            case gltf::TriangleReadStatus::ok:
                primitives.back() = std::move(triangles);
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
    }
    const SceneScale scene = sceneScale(document, reader, members, primitives);
    std::size_t read = 0;
    for (const auto& primitive : primitives) {
        read += primitive ? 1U : 0U;
    }
    const common::LargeArray<PositionKey> shared =
        read > 1 ? sharedPositions(primitives, workspace) : common::LargeArray<PositionKey>();

    std::vector<std::pair<std::size_t, std::size_t>> removed;
    for (std::size_t i = 0; i < primitives.size(); ++i) {
        const std::optional<gltf::TriangleData>& entry = primitives[i];
        if (!entry) {
            continue;
        }
        const auto [m, p] = members[i];
        const gltf::TriangleData& source = *entry;
        const common::LargeArray<float>& positions = source.position().values;
        ++report.input.primitives;
        report.input.vertices += source.vertex_count;
        report.input.faces += source.indices.size() / 3;
        // The limit in the mesh's own units: a node that scales the mesh up
        // makes it smaller there. The largest scale among the mesh's nodes
        // gives the strictest limit.
        const double mesh_scale = scene.mesh_scale[m];
        const bool measurable = scene.extent > 0 && mesh_scale > 0;
        const auto error = measurable ? static_cast<float>(options.error * scene.extent / mesh_scale) : 0.0F;

        common::LargeArray<std::uint8_t> lock;
        if (!shared.empty()) {
            lock = common::LargeArray<std::uint8_t>(workspace, source.vertex_count);
            for (std::size_t v = 0; v < source.vertex_count; ++v) {
                lock[v] = std::binary_search(shared.begin(), shared.end(), keyOf(&positions[3 * v])) ? 1 : 0;
            }
        }
        common::LargeArray<std::uint32_t> indices(workspace, source.indices.size());
        float result_error = 0;
        // No target count: collapse until the error bound, best effort, as
        // glTF-Transform's simplify does for eCorpus, with the error measured
        // on the whole scene. Disconnected parts smaller than the error, such
        // as floating scan noise, are removed (meshopt_SimplifyPrune): no
        // collapse could keep them at that size.
        const std::size_t count =
            measurable ? meshopt_simplifyWithAttributes(indices.data(), source.indices.data(), source.indices.size(),
                                                        positions.data(), source.vertex_count, 3 * sizeof(float),
                                                        nullptr, 0, nullptr, 0, lock.empty() ? nullptr : lock.data(), 0,
                                                        error, meshopt_SimplifyErrorAbsolute | meshopt_SimplifyPrune,
                                                        &result_error)
                       : source.indices.size();
        if (count == 0) {
            // Nothing left to draw: the primitive goes, and prune takes what
            // only it used.
            ++report.removed;
            removed.emplace_back(m, p);
            diagnostics.warn("primitive-removed", "Primitives smaller than the error were removed",
                             primitiveName(m, p));
            continue;
        }
        if (count == source.indices.size()) {
            ++report.unchanged;
            ++report.output.primitives;
            report.output.vertices += source.vertex_count;
            report.output.faces += source.indices.size() / 3;
            continue;
        }
        indices.shrink(count);
        written.push_back(gltf::compactVertices(source, std::move(indices), workspace));
        gltf::TriangleData& result = written.back();
        ++report.simplified;
        ++report.output.primitives;
        report.output.vertices += result.vertex_count;
        report.output.faces += result.indices.size() / 3;
        report.achieved_error =
            std::max(report.achieved_error, static_cast<double>(result_error) * mesh_scale / scene.extent);
        gltf::replaceGeometry(layout, m, p, gltf::addPlainGeometry(layout, result));
    }
    // Last primitive first, so that indices still to remove stay valid.
    std::sort(removed.begin(), removed.end(), std::greater<>());
    for (const auto& [m, p] : removed) {
        gltf::removePrimitive(layout, m, p);
    }
    gltf::prune(layout);
    report.files = gltf::writeSeparate(document, layout, directory, stem, outputs);
    return report;
}

} // namespace meshtinker::simplify
