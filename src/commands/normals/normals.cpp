#include "normals.hpp"

#include "meshtinker/common/large_array.hpp"
#include "meshtinker/gltf/edit.hpp"
#include "meshtinker/gltf/geometry.hpp"
#include "meshtinker/gltf/layout.hpp"
#include "meshtinker/gltf/meshopt_memory.hpp"
#include "meshtinker/gltf/primitive_data.hpp"
#include "meshtinker/gltf/transform.hpp"

#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <utility>

namespace meshtinker::normals {
namespace {

using Vector = std::array<double, 3>;

constexpr std::uint32_t unset = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint64_t max_count = std::numeric_limits<std::uint32_t>::max() - 1;
constexpr double pi = 3.14159265358979323846;
// A run of more than this many faces on one edge is unioned in a chain, not
// pair by pair: only degenerate files have one.
constexpr std::size_t max_pairs_per_edge = 32;

Vector sub(const float* a, const float* b) {
    return {static_cast<double>(a[0]) - b[0], static_cast<double>(a[1]) - b[1], static_cast<double>(a[2]) - b[2]};
}

Vector cross(const Vector& a, const Vector& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

double dot(const Vector& a, const Vector& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

double length(const Vector& a) { return std::sqrt(dot(a, a)); }

std::string primitiveName(std::size_t mesh, std::size_t primitive) {
    return "mesh " + std::to_string(mesh) + " primitive " + std::to_string(primitive);
}

enum class Need : std::uint8_t { missing, flat };

// A primitive that gets normals, read whole.
struct Candidate {
    std::size_t mesh = 0;
    std::size_t primitive = 0;
    Need need = Need::missing;
    gltf::TriangleData data;
    // Three per corner, once computed.
    common::LargeArray<float> corner_normals;
};

// Whether the three normals of every triangle are equal (−0 and 0 alike):
// one normal per face, which renders flat. Close isn't enough: on dense
// scans, smooth normals are within 1° of their face's on a third of the
// triangles (docs/experiments/normals.md, case 8). False if the primitive
// draws no triangle or its normals can't be read.
bool flatNormals(gltf::GeometryReader& reader, std::size_t mesh, std::size_t index, const cgltf_primitive& primitive,
                 const cgltf_accessor& normal) {
    if (reader.preparePrimitive(mesh, index) != gltf::Readability::readable ||
        reader.prepare(normal) != gltf::Readability::readable ||
        (primitive.indices != nullptr && reader.prepare(*primitive.indices) != gltf::Readability::readable)) {
        return false;
    }
    const std::uint64_t triangles = gltf::triangleCount(primitive.type, gltf::cornerCount(primitive));
    if (triangles == 0 || normal.type != cgltf_type_vec3) {
        return false;
    }
    for (std::uint64_t t = 0; t < triangles; ++t) {
        const std::array<std::uint64_t, 3> corners = gltf::triangleCorners(primitive.type, t);
        std::array<std::array<float, 3>, 3> values{};
        for (std::size_t k = 0; k < 3; ++k) {
            const std::size_t vertex = primitive.indices != nullptr
                                           ? cgltf_accessor_read_index(primitive.indices, corners[k])
                                           : static_cast<std::size_t>(corners[k]);
            if (vertex >= normal.count || cgltf_accessor_read_float(&normal, vertex, values[k].data(), 3) == 0) {
                return false;
            }
        }
        // Float comparison: −0 equals 0, and NaN equals nothing.
        if (values[0] != values[1] || values[0] != values[2]) {
            return false;
        }
    }
    return true;
}

// Groups of meshes whose vertices are in the same space: drawn by nodes with
// the same world matrices. Their primitives are smoothed together, so that
// tiles of one scan, as primitives or as meshes, show no seams. A mesh drawn
// by an instancing node, or by none, is a group of its own.
std::vector<std::size_t> meshGroups(const cgltf_data& data) {
    std::vector<std::vector<gltf::Matrix>> draws(data.meshes_count);
    std::vector<bool> alone(data.meshes_count, false);
    for (std::size_t n = 0; n < data.nodes_count; ++n) {
        const cgltf_node& node = data.nodes[n];
        if (node.mesh == nullptr) {
            continue;
        }
        const auto m = static_cast<std::size_t>(node.mesh - data.meshes);
        draws[m].push_back(gltf::worldMatrix(node));
        alone[m] = alone[m] || node.has_mesh_gpu_instancing != 0;
    }
    std::map<std::vector<gltf::Matrix>, std::size_t> groups;
    std::vector<std::size_t> result(data.meshes_count);
    std::size_t next = 0;
    for (std::size_t m = 0; m < data.meshes_count; ++m) {
        std::sort(draws[m].begin(), draws[m].end());
        if (alone[m] || draws[m].empty()) {
            result[m] = next++;
            continue;
        }
        const auto [entry, added] = groups.emplace(draws[m], next);
        next += added ? 1 : 0;
        result[m] = entry->second;
    }
    return result;
}

// One corner around a position, while its normal is worked out.
struct Wedge {
    // Its face's unit normal, and the corner's angle, its weight.
    Vector normal{};
    double angle = 0;
    bool degenerate = false;
    // The positions of its face's two other corners.
    std::array<std::uint32_t, 2> others{};
    std::uint32_t member = 0;
    std::size_t corner = 0;
};

std::uint32_t find(std::vector<std::uint32_t>& parent, std::uint32_t a) {
    while (parent[a] != a) {
        parent[a] = parent[parent[a]];
        a = parent[a];
    }
    return a;
}

// Computes one normal per corner for every member of a group of primitives in
// one space (docs/normals.md#how-normals-are-computed):
//
// - Corners are grouped by position, compared bit for bit, across members:
//   vertices duplicated at UV seams or along tile borders count as one.
// - Around a position, faces that share an edge join when their normals are
//   at most `crease` apart. Each set of joined faces gets one normal: the sum
//   of its faces' unit normals, each weighted by its corner's angle (as
//   Blender's smooth normals are). Every corner of the set gets the same
//   value, bit for bit, so that `weld` can merge their vertices.
// - Corners of zero-area faces take the normal of the heaviest set at their
//   position, so that they can merge too.
void computeNormals(std::vector<Candidate*>& members, double crease, common::Workspace& workspace) {
    std::vector<std::uint64_t> vertex_offsets{0};
    std::vector<std::uint64_t> corner_offsets{0};
    for (const Candidate* member : members) {
        vertex_offsets.push_back(vertex_offsets.back() + member->data.vertex_count);
        corner_offsets.push_back(corner_offsets.back() + member->data.indices.size());
    }
    const auto vertices = static_cast<std::size_t>(vertex_offsets.back());
    const auto corners = static_cast<std::size_t>(corner_offsets.back());

    // A position id per vertex, −0 counting as 0.
    common::LargeArray<std::uint32_t> position_of(workspace, vertices);
    std::size_t positions = 0;
    {
        common::LargeArray<float> all(workspace, 3 * vertices);
        for (std::size_t i = 0; i < members.size(); ++i) {
            const common::LargeArray<float>& values = members[i]->data.position().values;
            float* out = all.data() + 3 * vertex_offsets[i];
            for (std::size_t k = 0; k < values.size(); ++k) {
                out[k] = values[k] == 0.0F ? 0.0F : values[k];
            }
        }
        positions = meshopt_generateVertexRemap(position_of.data(), nullptr, vertices, all.data(), vertices,
                                                3 * sizeof(float));
    }
    common::LargeArray<std::uint32_t> corner_position(workspace, corners);
    for (std::size_t i = 0; i < members.size(); ++i) {
        const common::LargeArray<std::uint32_t>& indices = members[i]->data.indices;
        for (std::size_t c = 0; c < indices.size(); ++c) {
            corner_position[corner_offsets[i] + c] = position_of[vertex_offsets[i] + indices[c]];
        }
    }
    position_of = common::LargeArray<std::uint32_t>();

    // Corners sorted by position (a counting sort): `start[p]` is the first
    // of position p's.
    common::LargeArray<std::uint32_t> start(workspace, positions + 1);
    for (const std::uint32_t p : corner_position) {
        ++start[p + 1];
    }
    for (std::size_t p = 0; p < positions; ++p) {
        start[p + 1] += start[p];
    }
    common::LargeArray<std::uint32_t> order(workspace, corners);
    {
        common::LargeArray<std::uint32_t> next(workspace, positions);
        std::copy(start.begin(), start.begin() + static_cast<std::ptrdiff_t>(positions), next.begin());
        for (std::size_t c = 0; c < corners; ++c) {
            order[next[corner_position[c]]++] = static_cast<std::uint32_t>(c);
        }
    }

    for (Candidate* member : members) {
        member->corner_normals = common::LargeArray<float>(workspace, 3 * member->data.indices.size());
    }
    const double min_dot = std::cos(crease * pi / 180);
    const bool never_crease = crease >= 180;
    std::vector<Wedge> wedges;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
    std::vector<std::uint32_t> parent;
    std::vector<Vector> sums;
    std::vector<double> weights;
    for (std::size_t p = 0; p < positions; ++p) {
        wedges.clear();
        for (std::size_t o = start[p]; o < start[p + 1]; ++o) {
            const std::size_t c = order[o];
            const auto member = static_cast<std::uint32_t>(
                std::upper_bound(corner_offsets.begin(), corner_offsets.end(), c) - corner_offsets.begin() - 1);
            const gltf::TriangleData& data = members[member]->data;
            const std::size_t local = c - corner_offsets[member];
            const std::size_t face = local / 3 * 3;
            const std::size_t k = local % 3;
            const std::size_t next = face + (k + 1) % 3;
            const std::size_t previous = face + (k + 2) % 3;
            const float* positions_data = data.position().values.data();
            const float* here = positions_data + 3 * std::size_t{data.indices[local]};
            const Vector to_next = sub(positions_data + 3 * std::size_t{data.indices[next]}, here);
            const Vector to_previous = sub(positions_data + 3 * std::size_t{data.indices[previous]}, here);
            const Vector normal = cross(to_next, to_previous);
            const double area = length(normal);
            Wedge wedge;
            wedge.member = member;
            wedge.corner = local;
            wedge.others = {corner_position[corner_offsets[member] + next],
                            corner_position[corner_offsets[member] + previous]};
            wedge.degenerate = !(area > 0) || !std::isfinite(area) || wedge.others[0] == p || wedge.others[1] == p ||
                               wedge.others[0] == wedge.others[1];
            if (!wedge.degenerate) {
                wedge.normal = {normal[0] / area, normal[1] / area, normal[2] / area};
                wedge.angle = std::atan2(area, dot(to_next, to_previous));
            }
            wedges.push_back(wedge);
        }

        // Faces joined across the edges they share at this position.
        const auto count = static_cast<std::uint32_t>(wedges.size());
        parent.resize(count);
        for (std::uint32_t w = 0; w < count; ++w) {
            parent[w] = w;
        }
        edges.clear();
        for (std::uint32_t w = 0; w < count; ++w) {
            if (!wedges[w].degenerate) {
                edges.emplace_back(wedges[w].others[0], w);
                edges.emplace_back(wedges[w].others[1], w);
            }
        }
        std::sort(edges.begin(), edges.end());
        for (std::size_t i = 0; i < edges.size();) {
            std::size_t j = i + 1;
            while (j < edges.size() && edges[j].first == edges[i].first) {
                ++j;
            }
            for (std::size_t a = i; a + 1 < j; ++a) {
                const std::size_t last = j - a > max_pairs_per_edge ? a + 2 : j;
                for (std::size_t b = a + 1; b < last; ++b) {
                    const Wedge& first = wedges[edges[a].second];
                    const Wedge& second = wedges[edges[b].second];
                    if (never_crease || dot(first.normal, second.normal) >= min_dot) {
                        parent[find(parent, edges[a].second)] = find(parent, edges[b].second);
                    }
                }
            }
            i = j;
        }

        sums.assign(count, Vector{0, 0, 0});
        weights.assign(count, 0);
        std::uint32_t heaviest = unset;
        for (std::uint32_t w = 0; w < count; ++w) {
            if (wedges[w].degenerate) {
                continue;
            }
            const std::uint32_t root = find(parent, w);
            for (std::size_t k = 0; k < 3; ++k) {
                sums[root][k] += wedges[w].angle * wedges[w].normal[k];
            }
            weights[root] += wedges[w].angle;
            if (heaviest == unset || weights[root] > weights[heaviest]) {
                heaviest = root;
            }
        }
        for (std::uint32_t w = 0; w < count; ++w) {
            std::uint32_t root = find(parent, w);
            if (wedges[w].degenerate && heaviest != unset) {
                root = heaviest;
            }
            // Faces that cancel out (only without creases), or a position
            // that only zero-area faces draw: a face normal of the set, or +Y.
            Vector normal = sums[root];
            double norm = length(normal);
            if (!(norm > 0)) {
                normal = {0, 1, 0};
                for (std::uint32_t v = 0; v < count; ++v) {
                    if (!wedges[v].degenerate && find(parent, v) == root) {
                        normal = wedges[v].normal;
                        break;
                    }
                }
                norm = 1;
            }
            float* out = members[wedges[w].member]->corner_normals.data() + 3 * wedges[w].corner;
            for (std::size_t k = 0; k < 3; ++k) {
                out[k] = static_cast<float>(normal[k] / norm);
            }
        }
    }
}

// A vertex that carries a second normal, for one of its corners.
struct Split {
    std::uint32_t vertex = 0;
    std::array<float, 3> normal{};
    std::uint32_t corner = 0;
};

bool splitLess(const Split& a, const Split& b) {
    return a.vertex != b.vertex ? a.vertex < b.vertex : a.normal < b.normal;
}

// `source` with the normal of each corner: a vertex whose corners got
// different normals (on both sides of a crease) is split in as many. TANGENT
// is dropped. Vertices no triangle uses are dropped (compactVertices).
gltf::TriangleData withNormals(const gltf::TriangleData& source, const common::LargeArray<float>& corner_normals,
                               common::Workspace& workspace) {
    const std::size_t vertices = source.vertex_count;
    const std::size_t corners = source.indices.size();
    common::LargeArray<float> normal_of(workspace, 3 * vertices);
    common::LargeArray<std::uint8_t> assigned(workspace, vertices);
    std::size_t split_count = 0;
    const auto same = [&](std::size_t vertex, std::size_t corner) {
        return normal_of[3 * vertex] == corner_normals[3 * corner] &&
               normal_of[3 * vertex + 1] == corner_normals[3 * corner + 1] &&
               normal_of[3 * vertex + 2] == corner_normals[3 * corner + 2];
    };
    for (std::size_t c = 0; c < corners; ++c) {
        const std::size_t v = source.indices[c];
        if (assigned[v] == 0) {
            assigned[v] = 1;
            std::copy(&corner_normals[3 * c], &corner_normals[3 * c] + 3, &normal_of[3 * v]);
        } else if (!same(v, c)) {
            ++split_count;
        }
    }
    common::LargeArray<Split> splits(workspace, split_count);
    std::size_t s = 0;
    for (std::size_t c = 0; c < corners && s < split_count; ++c) {
        const std::size_t v = source.indices[c];
        if (!same(v, c)) {
            splits[s++] = {source.indices[c], {corner_normals[3 * c], corner_normals[3 * c + 1], corner_normals[3 * c + 2]},
                           static_cast<std::uint32_t>(c)};
        }
    }
    std::sort(splits.begin(), splits.end(), splitLess);
    common::LargeArray<std::uint32_t> indices(workspace, corners);
    std::copy(source.indices.begin(), source.indices.end(), indices.begin());
    // New vertices go after the source's, one per vertex and normal.
    std::size_t added = 0;
    for (std::size_t i = 0; i < split_count; ++i) {
        if (i == 0 || splitLess(splits[i - 1], splits[i])) {
            ++added;
        }
        indices[splits[i].corner] = static_cast<std::uint32_t>(vertices + added - 1);
    }
    common::LargeArray<std::uint32_t> copied(workspace, added);
    common::LargeArray<float> added_normals(workspace, 3 * added);
    for (std::size_t i = 0, a = 0; i < split_count; ++i) {
        if (i == 0 || splitLess(splits[i - 1], splits[i])) {
            copied[a] = splits[i].vertex;
            std::copy(splits[i].normal.begin(), splits[i].normal.end(), &added_normals[3 * a]);
            ++a;
        }
    }

    gltf::TriangleData expanded;
    expanded.vertex_count = vertices + added;
    expanded.indices_source = source.indices_source;
    const auto expand = [&](const gltf::AttributeData& attribute) {
        gltf::AttributeData out;
        out.name = attribute.name;
        out.components = attribute.components;
        out.source_component_type = attribute.source_component_type;
        out.source_accessor = attribute.source_accessor;
        const std::size_t n = attribute.components;
        out.values = common::LargeArray<float>(workspace, n * (vertices + added));
        std::copy(attribute.values.begin(), attribute.values.end(), out.values.begin());
        for (std::size_t a = 0; a < added; ++a) {
            std::copy(&attribute.values[n * copied[a]], &attribute.values[n * copied[a]] + n,
                      &out.values[n * (vertices + a)]);
        }
        return out;
    };
    gltf::AttributeData normal;
    normal.name = "NORMAL";
    normal.components = 3;
    normal.values = common::LargeArray<float>(workspace, 3 * (vertices + added));
    for (std::size_t v = 0; v < vertices; ++v) {
        const bool known = assigned[v] != 0;
        // Unused vertices are dropped: any unit vector does.
        normal.values[3 * v] = known ? normal_of[3 * v] : 0.0F;
        normal.values[3 * v + 1] = known ? normal_of[3 * v + 1] : 1.0F;
        normal.values[3 * v + 2] = known ? normal_of[3 * v + 2] : 0.0F;
    }
    std::copy(added_normals.begin(), added_normals.end(), &normal.values[3 * vertices]);
    for (const gltf::AttributeData& attribute : source.attributes) {
        if (attribute.name == "NORMAL") {
            normal.source_accessor = attribute.source_accessor;
            continue;
        }
        // Tangents are built on the normals they came with; without them,
        // viewers compute their own from the new normals (spec 3.7.2.1).
        if (attribute.name == "TANGENT") {
            continue;
        }
        expanded.attributes.push_back(expand(attribute));
        if (attribute.name == "POSITION") {
            expanded.attributes.emplace_back();
        }
    }
    *std::find_if(expanded.attributes.begin(), expanded.attributes.end(),
                  [](const gltf::AttributeData& a) { return a.name.empty(); }) = std::move(normal);

    return gltf::compactVertices(expanded, std::move(indices), workspace);
}

} // namespace

Report normals(const std::filesystem::path& input, const std::filesystem::path& directory, const std::string& stem,
               const Options& options, common::Workspace& workspace, common::Outputs& outputs,
               common::Diagnostics& diagnostics) {
    gltf::Document document = gltf::readNormalized(input, options.read, diagnostics);
    gltf::Layout layout = gltf::normalizedLayout(document, gltf::selectScene(document), diagnostics);
    gltf::GeometryReader reader(document, workspace);
    const gltf::MeshoptMemory meshopt_memory(workspace);
    const cgltf_data& data = document.data();
    Report report;
    report.crease_angle = options.crease_angle;

    std::deque<Candidate> candidates;
    for (std::size_t m = 0; m < data.meshes_count; ++m) {
        for (std::size_t p = 0; p < data.meshes[m].primitives_count; ++p) {
            const cgltf_primitive& primitive = data.meshes[m].primitives[p];
            if (gltf::triangleCount(primitive.type, 3) == 0) {
                // Points and lines aren't shaded by face.
                continue;
            }
            if (primitive.material != nullptr && primitive.material->unlit != 0) {
                ++report.unlit;
                continue;
            }
            Need need = Need::missing;
            if (const cgltf_accessor* normal = gltf::findAttribute(primitive, cgltf_attribute_type_normal, 0);
                normal != nullptr) {
                if (options.keep_flat || !flatNormals(reader, m, p, primitive, *normal)) {
                    ++report.kept;
                    continue;
                }
                need = Need::flat;
            }
            if (primitive.targets_count > 0) {
                ++report.skipped;
                diagnostics.warn("morph-targets-kept",
                                 "Primitives with morph targets were copied without normals of their own",
                                 primitiveName(m, p));
                continue;
            }
            Candidate candidate;
            candidate.mesh = m;
            candidate.primitive = p;
            candidate.need = need;
            const gltf::TriangleReadStatus status = gltf::readTriangles(document, reader, workspace, m, p, candidate.data);
            if (status != gltf::TriangleReadStatus::ok) {
                ++report.skipped;
                diagnostics.warn("normals-not-written",
                                 "Primitives whose geometry can't be read were copied without normals",
                                 primitiveName(m, p));
                continue;
            }
            if (candidate.data.indices.empty()) {
                ++report.kept;
                continue;
            }
            candidates.push_back(std::move(candidate));
        }
    }

    // Groups of candidates in one space, each within 32-bit vertex and
    // corner counts: beyond, a primitive is smoothed on its own.
    const std::vector<std::size_t> mesh_group = meshGroups(data);
    std::map<std::size_t, std::vector<Candidate*>> groups;
    std::map<std::size_t, std::pair<std::uint64_t, std::uint64_t>> sizes;
    std::vector<std::vector<Candidate*>> alone;
    for (Candidate& candidate : candidates) {
        const std::size_t group = mesh_group[candidate.mesh];
        auto& [vertices, corners] = sizes[group];
        if (vertices + candidate.data.vertex_count > max_count || corners + candidate.data.indices.size() > max_count) {
            alone.push_back({&candidate});
            continue;
        }
        vertices += candidate.data.vertex_count;
        corners += candidate.data.indices.size();
        groups[group].push_back(&candidate);
    }
    for (auto& [group, members] : groups) {
        alone.push_back(std::move(members));
    }

    // Rewritten primitives, whose arrays the written views read.
    std::deque<gltf::TriangleData> written;
    for (std::vector<Candidate*>& members : alone) {
        computeNormals(members, options.crease_angle, workspace);
        for (Candidate* candidate : members) {
            ++(candidate->need == Need::missing ? report.added : report.smoothed);
            ++report.input.primitives;
            report.input.vertices += candidate->data.vertex_count;
            report.input.faces += candidate->data.indices.size() / 3;
            written.push_back(withNormals(candidate->data, candidate->corner_normals, workspace));
            candidate->data = gltf::TriangleData();
            candidate->corner_normals = common::LargeArray<float>();
            gltf::TriangleData& result = written.back();
            ++report.output.primitives;
            report.output.vertices += result.vertex_count;
            report.output.faces += result.indices.size() / 3;
            gltf::replaceGeometry(layout, candidate->mesh, candidate->primitive, gltf::addPlainGeometry(layout, result));
        }
    }
    gltf::prune(layout);
    report.files = gltf::writeSeparate(document, layout, directory, stem, outputs);
    return report;
}

} // namespace meshtinker::normals
