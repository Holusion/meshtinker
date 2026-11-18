#include "voxel.hpp"

#include "meshtinker/common/failure.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <optional>
#include <thread>
#include <utility>

// Voxelization follows objtools' placeholder (docs/placeholder.md): each cell
// accumulates the area and area-weighted position of the surface crossing
// it, the streaming idea of Lindstrom's out-of-core simplification
// (SIGGRAPH 2000) without its quadrics.

namespace meshtinker::placeholder {
namespace {

// Parts smaller than this fraction of the largest one's area are noise at
// placeholder scale.
constexpr double min_part_area = 0.005;
// A far-off floater squeezes the model into a corner of the grid, and
// removing it leaves the grid mostly empty. When the remaining parts span less
// than this share of the longest axis, the model is voxelized again on a grid
// fitted to them.
constexpr double refit_below = 0.9;
// A floater far enough to fit the model inside one cell takes two passes to
// find the model's real size, and one more to get its cell bounds tight.
constexpr unsigned max_passes = 4;
// Fixed-point scale of accumulated areas, per cell face. A cell overflows at
// 2^31 layers of surface; a face smaller than 2^-32 of a cell's face still
// weighs one unit.
constexpr double one = 4294967296.0;
// The largest plane section of a unit cube is sqrt(2). A face's share of a
// cell is capped there: only a face lying mostly outside a refitted grid can
// claim more, and it would overflow the fixed-point sums.
constexpr double max_share = 1.4142135623730951;

Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

double longest(const Vec3& min, const Vec3& max) {
    return std::max({max[0] - min[0], max[1] - min[1], max[2] - min[2]});
}

// The cell a grid coordinate falls in along one axis, clamped to [low, high].
// Converting a negative or huge double straight to an integer is undefined.
std::size_t cellOf(double coordinate, std::size_t low, std::size_t high) {
    const double floored = std::floor(coordinate);
    if (!(floored > static_cast<double>(low))) {
        return low;
    }
    if (floored >= static_cast<double>(high)) {
        return high;
    }
    return static_cast<std::size_t>(floored);
}

// Adds one triangle, in grid coordinates, to the inner cells it overlaps.
void addSpanning(const Lattice& lattice, std::vector<Cell>& cells, const Triangle& t, double area,
                 std::vector<std::size_t>& hits) {
    std::array<std::size_t, 3> lo{};
    std::array<std::size_t, 3> hi{};
    for (std::size_t k = 0; k < 3; ++k) {
        const double min = std::min({t[0][k], t[1][k], t[2][k]});
        const double max = std::max({t[0][k], t[1][k], t[2][k]});
        const std::size_t top = lattice.dims[k] - 2;
        // Refitted grids leave out whatever lies beyond them. The tolerance
        // keeps faces lying exactly on the bounds, whose coordinates may round
        // just past the last inner cell.
        if (max < 1 || min > static_cast<double>(top + 1) + 1e-6) {
            return;
        }
        lo[k] = cellOf(min, 1, top);
        hi[k] = cellOf(max, 1, top);
    }
    hits.clear();
    for (std::size_t z = lo[2]; z <= hi[2]; ++z) {
        for (std::size_t y = lo[1]; y <= hi[1]; ++y) {
            for (std::size_t x = lo[0]; x <= hi[0]; ++x) {
                const Vec3 center{static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5,
                                  static_cast<double>(z) + 0.5};
                if (overlaps(center, t)) {
                    hits.push_back(lattice.id(x, y, z));
                }
            }
        }
    }
    // Clamped onto a cell it only touches, a face on the bounds may miss by rounding.
    if (hits.empty()) {
        hits.push_back(lattice.id(lo[0], lo[1], lo[2]));
    }
    const double share = std::min(area / static_cast<double>(hits.size()), max_share);
    for (const std::size_t id : hits) {
        const std::array<std::size_t, 3> c = lattice.coords(id);
        const Vec3 corner{static_cast<double>(c[0]), static_cast<double>(c[1]), static_cast<double>(c[2])};
        const Vec3 p = closest({corner[0] + 0.5, corner[1] + 0.5, corner[2] + 0.5}, t);
        cells[id].deposit(sub(p, corner), share);
    }
}

struct Part {
    std::vector<Cell> cells;
    std::uint64_t invalid_faces = 0;
};

Part accumulate(const Lattice& lattice, const Model& model, std::uint64_t start, std::uint64_t end) {
    Part part;
    part.cells.assign(lattice.size(), Cell{});
    std::vector<std::size_t> hits;
    const double scale = 1 / lattice.cell;
    part.invalid_faces = model.triangles(start, end, [&](const Triangle& face) {
        Triangle t{};
        for (std::size_t c = 0; c < 3; ++c) {
            for (std::size_t k = 0; k < 3; ++k) {
                t[c][k] = (face[c][k] - lattice.origin[k]) * scale;
            }
        }
        const Vec3 n = cross(sub(t[1], t[0]), sub(t[2], t[0]));
        const double area = 0.5 * std::sqrt(dot(n, n));
        // Degenerate faces add nothing; non-finite ones can't be placed.
        if (!(area > 0) || !std::isfinite(area)) {
            return;
        }
        // Nearly every face of a dense mesh sits inside one cell: comparing its
        // corners' cells settles those without the separating-axis tests.
        bool single = true;
        std::array<std::size_t, 3> c{};
        for (std::size_t k = 0; k < 3 && single; ++k) {
            const double cell = std::floor(t[0][k]);
            single = cell >= 1 && cell < static_cast<double>(lattice.dims[k] - 1) && std::floor(t[1][k]) == cell &&
                     std::floor(t[2][k]) == cell;
            c[k] = single ? static_cast<std::size_t>(cell) : 0;
        }
        if (single) {
            Vec3 centroid{};
            for (std::size_t k = 0; k < 3; ++k) {
                centroid[k] = (t[0][k] + t[1][k] + t[2][k]) / 3 - static_cast<double>(c[k]);
            }
            part.cells[lattice.id(c[0], c[1], c[2])].deposit(centroid, area);
        } else {
            addSpanning(lattice, part.cells, t, area, hits);
        }
    });
    return part;
}

Part rasterize(const Lattice& lattice, const Model& model, unsigned threads, std::uint64_t thread_faces) {
    const std::uint64_t count = std::max<std::uint64_t>(
        1, std::min<std::uint64_t>(threads, model.faces() / thread_faces + 1));
    std::vector<Part> parts(count);
    std::vector<std::exception_ptr> errors(count);
    const auto work = [&](std::uint64_t i) {
        try {
            parts[i] = accumulate(lattice, model, model.faces() * i / count, model.faces() * (i + 1) / count);
        } catch (...) {
            errors[i] = std::current_exception();
        }
    };
    std::vector<std::thread> workers;
    for (std::uint64_t i = 1; i < count; ++i) {
        workers.emplace_back(work, i);
    }
    work(0);
    for (std::thread& worker : workers) {
        worker.join();
    }
    for (const std::exception_ptr& error : errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }
    Part merged = std::move(parts[0]);
    for (std::uint64_t i = 1; i < count; ++i) {
        for (std::size_t c = 0; c < merged.cells.size(); ++c) {
            merged.cells[c].merge(parts[i].cells[c]);
        }
        merged.invalid_faces += parts[i].invalid_faces;
        std::vector<Cell>().swap(parts[i].cells);
    }
    return merged;
}

// Calls `visit` for the cells sharing a face, edge or corner with `id`, and itself.
template <typename Visit>
void forEachNeighbour(const Lattice& lattice, std::size_t id, Visit&& visit) {
    const std::array<std::size_t, 3> c = lattice.coords(id);
    std::array<std::size_t, 3> lo{};
    std::array<std::size_t, 3> hi{};
    for (std::size_t k = 0; k < 3; ++k) {
        lo[k] = c[k] > 0 ? c[k] - 1 : 0;
        hi[k] = std::min(c[k] + 1, lattice.dims[k] - 1);
    }
    for (std::size_t z = lo[2]; z <= hi[2]; ++z) {
        for (std::size_t y = lo[1]; y <= hi[1]; ++y) {
            for (std::size_t x = lo[0]; x <= hi[0]; ++x) {
                visit(lattice.id(x, y, z));
            }
        }
    }
}

struct Kept {
    std::uint64_t dropped = 0;
    // Bounds of the remaining cells, if any remain.
    std::optional<std::pair<Vec3, Vec3>> bounds;
};

// Empties the cells of parts below min_part_area of the largest, parts being
// 26-connected groups of occupied cells.
Kept dropSmallParts(Grid& grid) {
    const Lattice& lattice = grid.lattice;
    constexpr std::uint32_t none = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> part(lattice.size(), none);
    std::vector<double> areas;
    std::vector<std::size_t> stack;
    for (std::size_t seed = 0; seed < lattice.size(); ++seed) {
        if (!grid.cells[seed].occupied() || part[seed] != none) {
            continue;
        }
        const auto label = static_cast<std::uint32_t>(areas.size());
        double area = 0;
        part[seed] = label;
        stack.push_back(seed);
        while (!stack.empty()) {
            const std::size_t id = stack.back();
            stack.pop_back();
            area += static_cast<double>(grid.cells[id].area);
            forEachNeighbour(lattice, id, [&](std::size_t n) {
                if (grid.cells[n].occupied() && part[n] == none) {
                    part[n] = label;
                    stack.push_back(n);
                }
            });
        }
        areas.push_back(area);
    }
    const double largest = areas.empty() ? 0 : *std::max_element(areas.begin(), areas.end());
    Kept kept;
    for (std::size_t id = 0; id < lattice.size(); ++id) {
        if (part[id] == none) {
            continue;
        }
        if (areas[part[id]] < min_part_area * largest) {
            grid.cells[id] = Cell{};
            continue;
        }
        const std::array<std::size_t, 3> c = lattice.coords(id);
        Vec3 lo{};
        Vec3 hi{};
        for (std::size_t k = 0; k < 3; ++k) {
            lo[k] = lattice.origin[k] + static_cast<double>(c[k]) * lattice.cell;
            hi[k] = lattice.origin[k] + static_cast<double>(c[k] + 1) * lattice.cell;
        }
        if (!kept.bounds) {
            kept.bounds = std::make_pair(lo, hi);
        }
        for (std::size_t k = 0; k < 3; ++k) {
            kept.bounds->first[k] = std::min(kept.bounds->first[k], lo[k]);
            kept.bounds->second[k] = std::max(kept.bounds->second[k], hi[k]);
        }
    }
    for (const double area : areas) {
        kept.dropped += area < min_part_area * largest ? 1U : 0U;
    }
    return kept;
}

} // namespace

Vec3 Cell::mean() const {
    const auto a = static_cast<double>(area);
    return {static_cast<double>(sum[0]) / a, static_cast<double>(sum[1]) / a, static_cast<double>(sum[2]) / a};
}

void Cell::deposit(const Vec3& relative, double surface) {
    const double weight = surface * one;
    for (std::size_t k = 0; k < 3; ++k) {
        // The closest point of a face crossing the cell lies within a fraction
        // of a cell of it; clamping only bounds a face that merely touches it.
        sum[k] += static_cast<std::int64_t>(std::clamp(relative[k], -1.0, 2.0) * weight);
    }
    area += std::max<std::int64_t>(static_cast<std::int64_t>(weight), 1);
}

void Cell::merge(const Cell& other) {
    for (std::size_t k = 0; k < 3; ++k) {
        sum[k] += other.sum[k];
    }
    area += other.area;
}

Lattice Lattice::around(const Vec3& min, const Vec3& max, unsigned resolution) {
    const double size = longest(min, max);
    if (!(size > 0) || !std::isfinite(size)) {
        throw common::rejected("no-extent", "the model has no extent to build a placeholder around");
    }
    Lattice lattice;
    lattice.cell = size / resolution;
    for (std::size_t k = 0; k < 3; ++k) {
        // At most `resolution` cells, give or take rounding.
        const double cells = std::ceil((max[k] - min[k]) / lattice.cell);
        lattice.dims[k] = static_cast<std::size_t>(std::clamp(cells, 1.0, static_cast<double>(resolution) + 1)) + 2;
        lattice.origin[k] = min[k] - lattice.cell;
    }
    return lattice;
}

Voxelization voxelize(const Model& model, unsigned resolution, unsigned threads, std::uint64_t thread_faces) {
    Vec3 min = model.min();
    Vec3 max = model.max();
    Voxelization result;
    for (unsigned pass = 1;; ++pass) {
        Grid grid{Lattice::around(min, max, resolution), {}};
        Part part = rasterize(grid.lattice, model, threads, thread_faces);
        grid.cells = std::move(part.cells);
        const Kept kept = dropSmallParts(grid);
        // Parts are only told apart at the resolution of the pass that finds
        // them, so a later pass may count others: the largest count is the one
        // known to be real.
        result.dropped_parts = std::max(result.dropped_parts, kept.dropped);
        if (!kept.bounds) {
            throw common::rejected("no-surface", "the model has no surface to build a placeholder around");
        }
        if (pass < max_passes &&
            longest(kept.bounds->first, kept.bounds->second) < refit_below * longest(min, max)) {
            min = kept.bounds->first;
            max = kept.bounds->second;
            continue;
        }
        result.grid = std::move(grid);
        result.invalid_faces = part.invalid_faces;
        result.passes = pass;
        return result;
    }
}

bool overlaps(const Vec3& center, const Triangle& t) {
    const std::array<Vec3, 3> v{sub(t[0], center), sub(t[1], center), sub(t[2], center)};
    for (std::size_t k = 0; k < 3; ++k) {
        if (std::min({v[0][k], v[1][k], v[2][k]}) > 0.5 || std::max({v[0][k], v[1][k], v[2][k]}) < -0.5) {
            return false;
        }
    }
    const std::array<Vec3, 3> e{sub(v[1], v[0]), sub(v[2], v[1]), sub(v[0], v[2])};
    const auto separated = [](const Vec3& axis, double p, double q, double s) {
        const double r = 0.5 * (std::abs(axis[0]) + std::abs(axis[1]) + std::abs(axis[2]));
        return std::min({p, q, s}) > r || std::max({p, q, s}) < -r;
    };
    const Vec3 n = cross(e[0], e[1]);
    const double d = dot(n, v[0]);
    if (separated(n, d, d, d)) {
        return false;
    }
    constexpr std::array<Vec3, 3> units{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    for (const Vec3& edge : e) {
        for (const Vec3& unit : units) {
            const Vec3 a = cross(unit, edge);
            if (separated(a, dot(a, v[0]), dot(a, v[1]), dot(a, v[2]))) {
                return false;
            }
        }
    }
    return true;
}

Vec3 closest(const Vec3& p, const Triangle& t) {
    const Vec3& a = t[0];
    const Vec3& b = t[1];
    const Vec3& c = t[2];
    const auto along = [](const Vec3& from, const Vec3& d, double s) {
        return Vec3{from[0] + d[0] * s, from[1] + d[1] * s, from[2] + d[2] * s};
    };
    const Vec3 ab = sub(b, a);
    const Vec3 ac = sub(c, a);
    const Vec3 ap = sub(p, a);
    const double d1 = dot(ab, ap);
    const double d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) {
        return a;
    }
    const Vec3 bp = sub(p, b);
    const double d3 = dot(ab, bp);
    const double d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) {
        return b;
    }
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        return along(a, ab, d1 / (d1 - d3));
    }
    const Vec3 cp = sub(p, c);
    const double d5 = dot(ab, cp);
    const double d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) {
        return c;
    }
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        return along(a, ac, d2 / (d2 - d6));
    }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) {
        return along(b, sub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }
    const double denominator = 1 / (va + vb + vc);
    return along(along(a, ab, vb * denominator), ac, vc * denominator);
}

} // namespace meshtinker::placeholder
