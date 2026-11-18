#pragma once

#include "meshtinker/common/diagnostics.hpp"
#include "meshtinker/gltf/document.hpp"
#include "meshtinker/gltf/geometry.hpp"
#include "meshtinker/gltf/transform.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace meshtinker::placeholder {

using Vec3 = std::array<double, 3>;
using Triangle = std::array<Vec3, 3>;

// One triangle primitive, read in place. Positions and indices are read
// directly when they are plain float32 VEC3 and 16- or 32-bit indices, the
// usual case for scans, and through cgltf's accessor functions otherwise
// (quantized, sparse, 8-bit indices).
struct Primitive {
    const cgltf_accessor* position = nullptr;
    const cgltf_accessor* indices = nullptr;
    cgltf_primitive_type type = cgltf_primitive_type_triangles;
    std::uint64_t triangles = 0;
    const std::uint8_t* position_data = nullptr;
    std::size_t position_stride = 0;
    const std::uint8_t* index_data = nullptr;
    std::size_t index_stride = 0;
    // 2 or 4 for direct reads of indices, 0 otherwise.
    std::size_t index_size = 0;
    // The bounds of its finite vertices, which may be empty (min > max).
    Vec3 min{};
    Vec3 max{};

    // The vertex at position `corner` of the index list (or vertex list).
    std::uint64_t vertex(std::uint64_t corner) const {
        if (indices == nullptr) {
            return corner;
        }
        if (index_size == 4) {
            std::uint32_t value = 0;
            std::memcpy(&value, index_data + corner * index_stride, 4);
            return value;
        }
        if (index_size == 2) {
            std::uint16_t value = 0;
            std::memcpy(&value, index_data + corner * index_stride, 2);
            return value;
        }
        return cgltf_accessor_read_index(indices, static_cast<cgltf_size>(corner));
    }

    Vec3 point(std::uint64_t vertex) const {
        float p[3] = {0, 0, 0};
        if (position_data != nullptr) {
            std::memcpy(p, position_data + vertex * position_stride, sizeof p);
        } else {
            cgltf_accessor_read_float(position, static_cast<cgltf_size>(vertex), p, 3);
        }
        return {p[0], p[1], p[2]};
    }

    // Triangle `t`, or nothing when an index is out of range.
    std::optional<Triangle> triangle(std::uint64_t t) const {
        const std::array<std::uint64_t, 3> corners = gltf::triangleCorners(type, t);
        Triangle result{};
        for (std::size_t c = 0; c < 3; ++c) {
            const std::uint64_t v = vertex(corners[c]);
            if (v >= position->count) {
                return std::nullopt;
            }
            result[c] = point(v);
        }
        return result;
    }
};

// A mesh node of the scene: which primitives it draws, and where.
struct Placement {
    std::size_t first = 0;
    std::size_t last = 0;
    // From the node's coordinates to the frame's, unless they are the same,
    // as they are for every node of most models, which then pay nothing.
    std::optional<gltf::Matrix> to_frame;
    std::optional<gltf::Instancing> instancing;
    std::uint64_t instances = 1;

    std::optional<gltf::Matrix> matrix(std::uint64_t instance) const {
        if (!instancing) {
            return to_frame;
        }
        const gltf::Matrix m = gltf::instanceMatrix(*instancing, static_cast<std::size_t>(instance));
        return to_frame ? gltf::multiply(*to_frame, m) : m;
    }
};

// The triangles a normalized document's scene shows at rest, read in place,
// in the coordinates of one frame: the first mesh node's (docs/placeholder.md,
// "Where the placeholder sits").
class Model {
public:
    // Prepares the data of every primitive and instancing attribute through
    // `reader` (Draco, meshopt). Primitives that can't be read, and points and
    // lines, are left out with a counted warning. Rejects a scene that shows
    // no triangles.
    static Model read(gltf::Document& document, gltf::GeometryReader& reader, common::Diagnostics& diagnostics);

    // The transform properties of the nodes from the scene's root down to the
    // frame's node, as the source wrote them, leaving out nodes without any.
    const std::vector<gltf::Json>& frame() const { return frame_; }
    // Bounds in frame coordinates: each primitive's box seen through each of
    // its placements, so slightly larger than the geometry under a rotation.
    const Vec3& min() const { return min_; }
    const Vec3& max() const { return max_; }
    // Faces the scene draws: an instanced mesh counts once per instance.
    std::uint64_t faces() const { return faces_; }

    // Calls `visit(triangle)` for faces [start, end) of the whole scene, in
    // order, in frame coordinates. Returns how many faces were skipped for an
    // index out of range.
    template <typename Visit>
    std::uint64_t triangles(std::uint64_t start, std::uint64_t end, Visit&& visit) const {
        std::uint64_t skipped = 0;
        std::uint64_t base = 0;
        for (const Placement& placement : placements_) {
            for (std::size_t p = placement.first; p < placement.last; ++p) {
                const Primitive& primitive = primitives_[p];
                const std::uint64_t total = primitive.triangles * placement.instances;
                const std::uint64_t first = std::max(start, base);
                const std::uint64_t last = std::min(end, base + total);
                if (first < last && primitive.triangles > 0) {
                    const std::uint64_t from = first - base;
                    const std::uint64_t to = last - base;
                    for (std::uint64_t instance = from / primitive.triangles;
                         instance <= (to - 1) / primitive.triangles; ++instance) {
                        const std::optional<gltf::Matrix> matrix = placement.matrix(instance);
                        const std::uint64_t at = instance * primitive.triangles;
                        const std::uint64_t t_end = std::min(to - at, primitive.triangles);
                        for (std::uint64_t t = from > at ? from - at : 0; t < t_end; ++t) {
                            std::optional<Triangle> triangle = primitive.triangle(t);
                            if (!triangle) {
                                ++skipped;
                                continue;
                            }
                            if (matrix) {
                                for (Vec3& corner : *triangle) {
                                    corner = gltf::apply(*matrix, corner);
                                }
                            }
                            visit(*triangle);
                        }
                    }
                }
                base += total;
            }
        }
        return skipped;
    }

private:
    std::vector<Primitive> primitives_;
    std::vector<Placement> placements_;
    std::vector<gltf::Json> frame_;
    Vec3 min_{};
    Vec3 max_{};
    std::uint64_t faces_ = 0;
};

} // namespace meshtinker::placeholder
