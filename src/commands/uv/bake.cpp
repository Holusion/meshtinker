#include "bake.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace meshtinker::uv {

namespace {

double cross(double ax, double ay, double bx, double by) { return ax * by - ay * bx; }

// The squared distance from (px, py) to the segment a–b.
double segmentDistance2(double px, double py, double ax, double ay, double bx, double by) {
    const double ex = bx - ax;
    const double ey = by - ay;
    const double length2 = ex * ex + ey * ey;
    double t = length2 > 0 ? ((px - ax) * ex + (py - ay) * ey) / length2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    const double dx = px - (ax + t * ex);
    const double dy = py - (ay + t * ey);
    return dx * dx + dy * dy;
}

// Whether (px, py) is inside the triangle or within `reach` of it.
bool near(double px, double py, const double* x, const double* y, double reach) {
    const double d0 = cross(x[1] - x[0], y[1] - y[0], px - x[0], py - y[0]);
    const double d1 = cross(x[2] - x[1], y[2] - y[1], px - x[1], py - y[1]);
    const double d2 = cross(x[0] - x[2], y[0] - y[2], px - x[2], py - y[2]);
    if ((d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0)) {
        return true;
    }
    const double r2 = reach * reach;
    return segmentDistance2(px, py, x[0], y[0], x[1], y[1]) <= r2 || segmentDistance2(px, py, x[1], y[1], x[2], y[2]) <= r2 ||
           segmentDistance2(px, py, x[2], y[2], x[0], y[0]) <= r2;
}

struct Level {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
    std::vector<std::uint8_t> mask;
};

// Push-pull: each coarser level averages the covered texels below it, then
// every empty texel takes its parent's value, coarsest first.
void fill(image::Image& image, std::vector<std::uint8_t>& mask) {
    std::vector<Level> levels;
    {
        Level base;
        base.width = image.width;
        base.height = image.height;
        base.pixels = std::move(image.pixels);
        base.mask = std::move(mask);
        levels.push_back(std::move(base));
    }
    while (levels.back().width > 1 || levels.back().height > 1) {
        const Level& fine = levels.back();
        Level coarse;
        coarse.width = std::max(1u, (fine.width + 1) / 2);
        coarse.height = std::max(1u, (fine.height + 1) / 2);
        coarse.pixels.assign(static_cast<std::size_t>(coarse.width) * coarse.height * 4, 0);
        coarse.mask.assign(static_cast<std::size_t>(coarse.width) * coarse.height, 0);
        for (std::uint32_t y = 0; y < coarse.height; ++y) {
            for (std::uint32_t x = 0; x < coarse.width; ++x) {
                std::uint32_t sum[4] = {0, 0, 0, 0};
                std::uint32_t count = 0;
                for (std::uint32_t j = 0; j < 2; ++j) {
                    for (std::uint32_t i = 0; i < 2; ++i) {
                        const std::uint32_t fx = 2 * x + i;
                        const std::uint32_t fy = 2 * y + j;
                        if (fx >= fine.width || fy >= fine.height) {
                            continue;
                        }
                        const std::size_t f = static_cast<std::size_t>(fy) * fine.width + fx;
                        if (fine.mask[f] == 0) {
                            continue;
                        }
                        for (std::size_t c = 0; c < 4; ++c) {
                            sum[c] += fine.pixels[f * 4 + c];
                        }
                        ++count;
                    }
                }
                if (count == 0) {
                    continue;
                }
                const std::size_t t = static_cast<std::size_t>(y) * coarse.width + x;
                coarse.mask[t] = 1;
                for (std::size_t c = 0; c < 4; ++c) {
                    coarse.pixels[t * 4 + c] = static_cast<std::uint8_t>((sum[c] + count / 2) / count);
                }
            }
        }
        levels.push_back(std::move(coarse));
    }
    for (std::size_t l = levels.size() - 1; l-- > 0;) {
        Level& fine = levels[l];
        const Level& coarse = levels[l + 1];
        for (std::uint32_t y = 0; y < fine.height; ++y) {
            for (std::uint32_t x = 0; x < fine.width; ++x) {
                const std::size_t f = static_cast<std::size_t>(y) * fine.width + x;
                if (fine.mask[f] != 0) {
                    continue;
                }
                const std::size_t c = static_cast<std::size_t>(y / 2) * coarse.width + x / 2;
                std::memcpy(&fine.pixels[f * 4], &coarse.pixels[c * 4], 4);
                fine.mask[f] = coarse.mask[c];
            }
        }
        levels.resize(l + 1);
    }
    image.pixels = std::move(levels.front().pixels);
}

} // namespace

Baked bake(const image::Image& source, std::uint32_t divisor, image::Size size, const std::vector<BakeTriangle>& triangles) {
    // Bilinear filtering at a point of the triangle reads the four texels
    // around it, whose centers are up to √2 texels away.
    constexpr double reach = 1.5;
    Baked baked;
    image::Image& out = baked.image;
    out.width = size.width;
    out.height = size.height;
    out.source_channels = source.source_channels;
    out.pixels.assign(out.texels() * 4, 0);
    std::vector<std::uint8_t> mask(out.texels(), 0);
    const double d = divisor;
    const auto sw = static_cast<std::int64_t>(source.width);
    const auto sh = static_cast<std::int64_t>(source.height);
    const auto ow = static_cast<std::int64_t>(out.width);
    const auto oh = static_cast<std::int64_t>(out.height);
    for (const BakeTriangle& t : triangles) {
        double x[3];
        double y[3];
        for (std::size_t i = 0; i < 3; ++i) {
            x[i] = static_cast<double>(t.x[i]) / d;
            y[i] = static_cast<double>(t.y[i]) / d;
        }
        // Texels whose centers are within `reach` of the triangle.
        const auto x0 = static_cast<std::int64_t>(std::floor(std::min({x[0], x[1], x[2]}) - reach - 0.5));
        const auto x1 = static_cast<std::int64_t>(std::ceil(std::max({x[0], x[1], x[2]}) + reach - 0.5));
        const auto y0 = static_cast<std::int64_t>(std::floor(std::min({y[0], y[1], y[2]}) - reach - 0.5));
        const auto y1 = static_cast<std::int64_t>(std::ceil(std::max({y[0], y[1], y[2]}) + reach - 0.5));
        const std::int64_t dx = t.dx / static_cast<std::int64_t>(divisor);
        const std::int64_t dy = t.dy / static_cast<std::int64_t>(divisor);
        for (std::int64_t j = y0; j <= y1; ++j) {
            for (std::int64_t i = x0; i <= x1; ++i) {
                const std::int64_t ox = i + dx;
                const std::int64_t oy = j + dy;
                if (ox < 0 || oy < 0 || ox >= ow || oy >= oh) {
                    continue;
                }
                if (!near(static_cast<double>(i) + 0.5, static_cast<double>(j) + 0.5, x, y, reach)) {
                    continue;
                }
                // Outside the source, sampling clamps to its edge.
                const std::int64_t si = std::clamp<std::int64_t>(i, 0, sw - 1);
                const std::int64_t sj = std::clamp<std::int64_t>(j, 0, sh - 1);
                const auto o = static_cast<std::size_t>(oy * ow + ox);
                const auto s = static_cast<std::size_t>(sj * sw + si);
                std::memcpy(&out.pixels[o * 4], &source.pixels[s * 4], 4);
                baked.covered += mask[o] == 0 ? 1u : 0u;
                mask[o] = 1;
            }
        }
    }
    fill(out, mask);
    return baked;
}

} // namespace meshtinker::uv
