#pragma once

#include "meshtinker/image/image.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace meshtinker::uv {

// A triangle of a chart: its corners in the source, in texels of the group's
// largest image ("fine" texels), and the chart's move, in whole fine texels
// that are a multiple of every image's divisor.
struct BakeTriangle {
    std::array<float, 3> x{};
    std::array<float, 3> y{};
    std::int64_t dx = 0;
    std::int64_t dy = 0;
};

struct Baked {
    image::Image image;
    // Texels a triangle covers.
    std::uint64_t covered = 0;
};

// Copies the texels of `source` that `triangles` cover, and those within 1.5
// texels of them (bilinear filtering reads them), to the triangles' new place
// in an image of `size`, then fills the rest by push-pull from the copied
// texels (docs/uv.md#atlas-size). `divisor` is the number of fine texels in one
// texel of `source`. Every move being whole texels, nothing is resampled.
Baked bake(const image::Image& source, std::uint32_t divisor, image::Size size, const std::vector<BakeTriangle>& triangles);

} // namespace meshtinker::uv
