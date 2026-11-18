#pragma once

#include "model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace meshtinker::placeholder {

// What one cell holds of the surface crossing it: its area, and its
// area-weighted position relative to the cell's lower corner, in cell units.
// Both are fixed-point integers (2^32 per cell face), so that sums don't depend
// on their order and thread counts give identical grids.
struct Cell {
    std::array<std::int64_t, 3> sum{};
    std::int64_t area = 0;

    bool occupied() const { return area > 0; }
    // Mean surface position relative to the cell's lower corner, in cell units.
    Vec3 mean() const;
    // Adds `surface` (in cell faces) of surface at `relative`.
    void deposit(const Vec3& relative, double surface);
    void merge(const Cell& other);
};

// Cubic cells over a box, with one empty layer all around for the exterior
// flood fill.
struct Lattice {
    std::array<std::size_t, 3> dims{};
    Vec3 origin{};
    double cell = 0;

    // Sized so that the box's longest side spans `resolution` cells. Rejects
    // a box without extent.
    static Lattice around(const Vec3& min, const Vec3& max, unsigned resolution);

    std::size_t id(std::size_t x, std::size_t y, std::size_t z) const { return x + dims[0] * (y + dims[1] * z); }
    std::size_t size() const { return dims[0] * dims[1] * dims[2]; }
    std::array<std::size_t, 3> coords(std::size_t id) const {
        return {id % dims[0], (id / dims[0]) % dims[1], id / (dims[0] * dims[1])};
    }
};

struct Grid {
    Lattice lattice;
    std::vector<Cell> cells;
};

struct Voxelization {
    Grid grid;
    // Parts left out as noise, at the largest count a pass found.
    std::uint64_t dropped_parts = 0;
    // Faces left out for an index out of range.
    std::uint64_t invalid_faces = 0;
    unsigned passes = 0;
};

// Each thread's grid costs memory, so small models don't use more threads
// than they need.
constexpr std::uint64_t faces_per_thread = 100000;

// Voxelizes the model's faces on `threads` threads, at most one per
// `faces_per_thread` faces, drops small disconnected parts, and voxelizes
// again on a grid fitted to what remains when that leaves much of the grid
// empty (docs/placeholder.md). The grid doesn't depend on the thread count.
Voxelization voxelize(const Model& model, unsigned resolution, unsigned threads,
                      std::uint64_t thread_faces = faces_per_thread);

// Triangle and unit-cube overlap, by separating axes (Akenine-Möller 2001),
// for the cube of half extent 0.5 around `center`.
bool overlaps(const Vec3& center, const Triangle& t);

// The point of `t` closest to `p` (Ericson, Real-Time Collision Detection,
// 5.1.5).
Vec3 closest(const Vec3& p, const Triangle& t);

} // namespace meshtinker::placeholder
