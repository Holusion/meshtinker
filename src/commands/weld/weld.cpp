#include "weld.hpp"

#include "meshtinker/common/large_array.hpp"
#include "meshtinker/gltf/edit.hpp"
#include "meshtinker/gltf/geometry.hpp"
#include "meshtinker/gltf/layout.hpp"
#include "meshtinker/gltf/meshopt_memory.hpp"
#include "meshtinker/gltf/primitive_data.hpp"

#include <meshoptimizer.h>

#include <algorithm>
#include <deque>
#include <limits>

namespace meshtinker::weld {
namespace {

constexpr std::uint32_t unset = std::numeric_limits<std::uint32_t>::max();

std::size_t indexOf(const cgltf_accessor& accessor, const cgltf_data& data) {
    return static_cast<std::size_t>(&accessor - data.accessors);
}

std::string primitiveName(std::size_t mesh, std::size_t primitive) {
    return "mesh " + std::to_string(mesh) + " primitive " + std::to_string(primitive);
}

// −0 becomes 0, so that the two compare equal bit for bit. Same value, so
// nothing a reader sees changes.
void clearNegativeZeros(gltf::AttributeData& attribute) {
    for (float& value : attribute.values) {
        if (value == 0.0F) {
            value = 0.0F;
        }
    }
}

void addStreams(std::vector<gltf::AttributeData>& attributes, std::vector<meshopt_Stream>& streams) {
    for (gltf::AttributeData& attribute : attributes) {
        clearNegativeZeros(attribute);
        const std::size_t size = attribute.components * sizeof(float);
        streams.push_back({attribute.values.data(), size, size});
    }
}

} // namespace

Report weld(const std::filesystem::path& input, const std::filesystem::path& directory, const std::string& stem,
            const Options& options, common::Workspace& workspace, common::Outputs& outputs,
            common::Diagnostics& diagnostics) {
    gltf::Document document = gltf::readNormalized(input, options.read, diagnostics);
    gltf::Layout layout = gltf::normalizedLayout(document, gltf::selectScene(document), diagnostics);
    gltf::GeometryReader reader(document, workspace);
    const gltf::MeshoptMemory meshopt_memory(workspace);
    const cgltf_data& data = document.data();
    Report report;

    // How many primitives read each accessor: welding one primitive of two
    // that share vertices would give it a copy of its own, and grow the file.
    std::vector<std::uint32_t> users(data.accessors_count, 0);
    for (std::size_t m = 0; m < data.meshes_count; ++m) {
        for (std::size_t p = 0; p < data.meshes[m].primitives_count; ++p) {
            gltf::forEachAccessor(data.meshes[m].primitives[p],
                                  [&](const cgltf_accessor& accessor) { ++users[indexOf(accessor, data)]; });
        }
    }

    // Welded primitives, whose arrays the written views read.
    std::deque<gltf::TriangleData> written;
    for (std::size_t m = 0; m < data.meshes_count; ++m) {
        for (std::size_t p = 0; p < data.meshes[m].primitives_count; ++p) {
            const cgltf_primitive& primitive = data.meshes[m].primitives[p];
            bool meshopt = false;
            bool shared = false;
            gltf::forEachAccessor(primitive, [&](const cgltf_accessor& accessor) {
                meshopt = meshopt || gltf::usesMeshopt(accessor);
                shared = shared || users[indexOf(accessor, data)] > 1;
            });
            if (gltf::GeometryReader::isDraco(primitive) || meshopt) {
                ++report.compressed;
                continue;
            }
            if (shared) {
                ++report.skipped;
                diagnostics.warn("shared-vertices-kept",
                                 "Primitives that share vertex data with another primitive were copied as they are",
                                 primitiveName(m, p));
                continue;
            }
            gltf::TriangleData source;
            switch (gltf::readTriangles(document, reader, workspace, m, p, source)) {
            case gltf::TriangleReadStatus::ok:
                break;
            case gltf::TriangleReadStatus::not_triangles:
                ++report.skipped;
                diagnostics.warn("points-and-lines-kept", "Points and lines were copied without welding",
                                 primitiveName(m, p));
                continue;
            case gltf::TriangleReadStatus::unsupported:
            case gltf::TriangleReadStatus::unreadable:
                ++report.skipped;
                diagnostics.warn("primitive-not-welded",
                                 "Primitives whose geometry can't be read were copied as they are",
                                 primitiveName(m, p));
                continue;
            }
            const std::size_t corners = source.indices.size();
            ++report.input.primitives;
            report.input.vertices += source.vertex_count;
            report.input.faces += corners / 3;

            // Every attribute and every morph target's, bit for bit, as
            // glTF-Transform's weld() compares them (meshoptimizer's remap).
            std::vector<meshopt_Stream> streams;
            addStreams(source.attributes, streams);
            for (std::vector<gltf::AttributeData>& target : source.targets) {
                addStreams(target, streams);
            }
            common::LargeArray<std::uint32_t> remap(workspace, source.vertex_count);
            const std::size_t unique =
                meshopt_generateVertexRemapMulti(remap.data(), source.indices.data(), corners, source.vertex_count,
                                                 streams.data(), streams.size());
            if (unique == source.vertex_count) {
                ++report.unchanged;
                ++report.output.primitives;
                report.output.vertices += source.vertex_count;
                report.output.faces += corners / 3;
                continue;
            }
            // Each corner points at the first vertex of its kind, which
            // compactVertices keeps, with all its values.
            common::LargeArray<std::uint32_t> first(workspace, unique);
            std::fill(first.begin(), first.end(), unset);
            common::LargeArray<std::uint32_t> indices(workspace, corners);
            for (std::size_t i = 0; i < corners; ++i) {
                const std::uint32_t vertex = source.indices[i];
                std::uint32_t& kept = first[remap[vertex]];
                if (kept == unset) {
                    kept = vertex;
                }
                indices[i] = kept;
            }
            written.push_back(gltf::compactVertices(source, std::move(indices), workspace));
            gltf::TriangleData& result = written.back();
            ++report.welded;
            ++report.output.primitives;
            report.output.vertices += result.vertex_count;
            report.output.faces += result.indices.size() / 3;
            gltf::replaceGeometry(layout, m, p, gltf::addPlainGeometry(layout, result));
        }
    }
    gltf::prune(layout);
    report.files = gltf::writeSeparate(document, layout, directory, stem, outputs);
    return report;
}

} // namespace meshtinker::weld
