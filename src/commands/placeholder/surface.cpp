#include "surface.hpp"

#include "meshtinker/common/failure.hpp"

#include <cmath>
#include <limits>

namespace meshtinker::placeholder {
namespace {

// Triangles below this area, in cell faces, are slivers from collapsed vertices.
constexpr double min_area = 1e-6;
constexpr std::uint32_t none = std::numeric_limits<std::uint32_t>::max();

Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double length(const Vec3& v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
double squaredDistance(const Vec3& a, const Vec3& b) {
    const Vec3 d = sub(a, b);
    return d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
}

// Cells the exterior reaches through shared faces, from the empty outer
// layer. Enclosed cavities aren't reached, so interior geometry and hidden
// chambers produce no surface.
std::vector<bool> exterior(const Grid& grid) {
    const Lattice& lattice = grid.lattice;
    std::vector<bool> outside(lattice.size(), false);
    outside[0] = true;
    std::vector<std::size_t> stack{0};
    while (!stack.empty()) {
        const std::size_t id = stack.back();
        stack.pop_back();
        const std::array<std::size_t, 3> c = lattice.coords(id);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            for (const bool high : {false, true}) {
                std::array<std::size_t, 3> n = c;
                if (high && c[axis] + 1 < lattice.dims[axis]) {
                    ++n[axis];
                } else if (!high && c[axis] > 0) {
                    --n[axis];
                } else {
                    continue;
                }
                const std::size_t neighbour = lattice.id(n[0], n[1], n[2]);
                if (!outside[neighbour] && !grid.cells[neighbour].occupied()) {
                    outside[neighbour] = true;
                    stack.push_back(neighbour);
                }
            }
        }
    }
    return outside;
}

// Mean surface position of the occupied cells sharing a lattice corner, in
// grid coordinates. A boundary face always has an occupied cell behind it,
// since the flood fill would otherwise have crossed it, so every corner the
// surface uses has at least one.
Vec3 cornerPosition(const Grid& grid, const std::array<std::size_t, 3>& corner) {
    const Lattice& lattice = grid.lattice;
    Vec3 sum{};
    double count = 0;
    std::array<std::size_t, 3> lo{};
    std::array<std::size_t, 3> hi{};
    for (std::size_t k = 0; k < 3; ++k) {
        lo[k] = corner[k] > 0 ? corner[k] - 1 : 0;
        hi[k] = std::min(corner[k], lattice.dims[k] - 1);
    }
    for (std::size_t z = lo[2]; z <= hi[2]; ++z) {
        for (std::size_t y = lo[1]; y <= hi[1]; ++y) {
            for (std::size_t x = lo[0]; x <= hi[0]; ++x) {
                const Cell& cell = grid.cells[lattice.id(x, y, z)];
                if (cell.occupied()) {
                    const Vec3 mean = cell.mean();
                    sum[0] += static_cast<double>(x) + mean[0];
                    sum[1] += static_cast<double>(y) + mean[1];
                    sum[2] += static_cast<double>(z) + mean[2];
                    count += 1;
                }
            }
        }
    }
    return {sum[0] / count, sum[1] / count, sum[2] / count};
}

} // namespace

Mesh extractSurface(const Grid& grid) {
    const Lattice& lattice = grid.lattice;
    const std::vector<bool> outside = exterior(grid);
    const std::array<std::size_t, 3> corners{lattice.dims[0] + 1, lattice.dims[1] + 1, lattice.dims[2] + 1};
    const auto latticeId = [&](const std::array<std::size_t, 3>& c) {
        return c[0] + corners[0] * (c[1] + corners[1] * c[2]);
    };
    std::vector<std::uint32_t> vertex_at(corners[0] * corners[1] * corners[2], none);
    std::vector<Vec3> vertices;
    std::vector<std::array<std::uint32_t, 3>> triangles;

    for (std::size_t id = 0; id < lattice.size(); ++id) {
        if (outside[id]) {
            continue;
        }
        const std::array<std::size_t, 3> cell = lattice.coords(id);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            for (const bool high : {false, true}) {
                std::array<std::size_t, 3> neighbour = cell;
                if (high && cell[axis] + 1 < lattice.dims[axis]) {
                    ++neighbour[axis];
                } else if (!high && cell[axis] > 0) {
                    --neighbour[axis];
                } else {
                    continue;
                }
                if (!outside[lattice.id(neighbour[0], neighbour[1], neighbour[2])]) {
                    continue;
                }
                const std::size_t u = (axis + 1) % 3;
                const std::size_t v = (axis + 2) % 3;
                std::array<std::array<std::size_t, 3>, 4> quad_corners{cell, cell, cell, cell};
                for (auto& c : quad_corners) {
                    c[axis] += high ? 1U : 0U;
                }
                ++quad_corners[1][u];
                ++quad_corners[2][u];
                ++quad_corners[2][v];
                ++quad_corners[3][v];
                // u x v points along +axis, so the low side reverses to face outward.
                if (!high) {
                    std::swap(quad_corners[1], quad_corners[3]);
                }
                std::array<std::uint32_t, 4> quad{};
                std::array<Vec3, 4> p{};
                for (std::size_t i = 0; i < 4; ++i) {
                    std::uint32_t& slot = vertex_at[latticeId(quad_corners[i])];
                    if (slot == none) {
                        slot = static_cast<std::uint32_t>(vertices.size());
                        vertices.push_back(cornerPosition(grid, quad_corners[i]));
                    }
                    quad[i] = slot;
                    p[i] = vertices[slot];
                }
                // The shorter diagonal gives better-shaped triangles once vertices have moved.
                const bool first_diagonal = squaredDistance(p[0], p[2]) <= squaredDistance(p[1], p[3]);
                const std::array<std::array<std::size_t, 3>, 2> split =
                    first_diagonal ? std::array<std::array<std::size_t, 3>, 2>{{{0, 1, 2}, {0, 2, 3}}}
                                   : std::array<std::array<std::size_t, 3>, 2>{{{0, 1, 3}, {1, 2, 3}}};
                for (const auto& [a, b, c] : split) {
                    if (0.5 * length(cross(sub(p[b], p[a]), sub(p[c], p[a]))) > min_area) {
                        triangles.push_back({quad[a], quad[b], quad[c]});
                    }
                }
            }
        }
    }
    if (triangles.empty()) {
        throw common::rejected("no-surface", "no surface remains at this resolution: try a higher --resolution");
    }

    // Smooth normals: each vertex sums its faces' area-weighted normals.
    std::vector<Vec3> normals(vertices.size(), Vec3{});
    for (const auto& t : triangles) {
        const Vec3 n = cross(sub(vertices[t[1]], vertices[t[0]]), sub(vertices[t[2]], vertices[t[0]]));
        for (const std::uint32_t i : t) {
            for (std::size_t k = 0; k < 3; ++k) {
                normals[i][k] += n[k];
            }
        }
    }
    // Slivers dropped above can orphan a vertex, so only referenced ones are written.
    Mesh mesh;
    std::vector<std::uint32_t> written(vertices.size(), none);
    mesh.indices.reserve(triangles.size() * 3);
    for (const auto& t : triangles) {
        for (const std::uint32_t i : t) {
            if (written[i] == none) {
                written[i] = static_cast<std::uint32_t>(mesh.positions.size());
                const Vec3& g = vertices[i];
                mesh.positions.push_back({static_cast<float>(lattice.origin[0] + g[0] * lattice.cell),
                                          static_cast<float>(lattice.origin[1] + g[1] * lattice.cell),
                                          static_cast<float>(lattice.origin[2] + g[2] * lattice.cell)});
                const double l = length(normals[i]);
                mesh.normals.push_back(l > 0 ? std::array<float, 3>{static_cast<float>(normals[i][0] / l),
                                                                     static_cast<float>(normals[i][1] / l),
                                                                     static_cast<float>(normals[i][2] / l)}
                                             : std::array<float, 3>{0, 1, 0});
            }
            mesh.indices.push_back(written[i]);
        }
    }
    return mesh;
}

} // namespace meshtinker::placeholder
