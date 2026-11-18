#pragma once

#include "voxel.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace meshtinker::placeholder {

struct Mesh {
    // In the frame's coordinates.
    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 3>> normals;
    std::vector<std::uint32_t> indices;
};

// The outer surface of the grid: Surface Nets (Gibson 1998) over binary
// cells, each vertex at the mean surface position of the occupied cells
// around it rather than at a lattice corner (docs/placeholder.md). Rejects a
// grid that leaves no surface.
Mesh extractSurface(const Grid& grid);

} // namespace meshtinker::placeholder
