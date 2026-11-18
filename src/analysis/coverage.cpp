#include "meshtinker/analysis/coverage.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace meshtinker::analysis {
namespace {

using Point = std::array<double, 2>;
using Span = std::optional<std::pair<std::int64_t, std::int64_t>>;

// Texel coordinates beyond this are rejected rather than overflowing integer texel indices.
constexpr double limit = 4503599627370496.0; // 2^52
// A triangle repeating the texture more often than this vertically is left out:
// its rows would be swept one by one. Horizontal repeats are cheap, since a row
// wider than two periods is full.
constexpr std::int64_t max_repeats = 64;

std::int64_t floorDiv(std::int64_t a, std::int64_t n) {
    const std::int64_t q = a / n;
    return (a % n != 0 && (a < 0) != (n < 0)) ? q - 1 : q;
}

std::int64_t floorMod(std::int64_t a, std::int64_t n) { return a - floorDiv(a, n) * n; }

// Extra texels read beyond the containing one, on the far side of each axis
// once the coordinates are shifted by half a texel.
std::int64_t reach(Filter filter) { return filter == Filter::linear ? 1 : 0; }

std::int64_t wrapIndex(Wrap wrap, std::int64_t i, std::int64_t n) {
    switch (wrap) {
    case Wrap::repeat:
        return floorMod(i, n);
    case Wrap::clamp:
        return std::clamp<std::int64_t>(i, 0, n - 1);
    case Wrap::mirror: {
        const std::int64_t m = floorMod(i, 2 * n);
        return m < n ? m : 2 * n - 1 - m;
    }
    }
    return 0;
}

// Maps the plane texel range [a, b) onto texture ranges within [0, n).
template <typename F>
void wrapRanges(Wrap wrap, std::int64_t a, std::int64_t b, std::int64_t n, F f) {
    if (wrap == Wrap::clamp) {
        f(std::clamp<std::int64_t>(a, 0, n - 1), std::clamp<std::int64_t>(b, 1, n));
        return;
    }
    if (b - a >= 2 * n) {
        f(0, n);
        return;
    }
    std::int64_t x = a;
    while (x < b) {
        const std::int64_t period = floorDiv(x, n);
        const std::int64_t end = std::min(b, (period + 1) * n);
        const std::int64_t s = x - period * n;
        const std::int64_t e = end - period * n;
        if (wrap == Wrap::mirror && floorMod(period, 2) == 1) {
            f(n - e, n - s);
        } else {
            f(s, e);
        }
        x = end;
    }
}

// Texel columns [floor(min x), ceil(max x) + reach) of the triangle's part with y0 <= y <= y1.
Span span(const std::array<Point, 3>& tri, double y0, double y1, std::int64_t reach_texels) {
    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < 3; ++i) {
        Point p = tri[i];
        Point q = tri[(i + 1) % 3];
        if (p[1] > q[1]) {
            std::swap(p, q);
        }
        if (q[1] < y0 || p[1] > y1) {
            continue;
        }
        double xa = 0;
        double xb = 0;
        if (q[1] == p[1]) {
            xa = p[0];
            xb = q[0];
        } else {
            const auto at = [&](double y) { return p[0] + (y - p[1]) / (q[1] - p[1]) * (q[0] - p[0]); };
            xa = at(std::max(p[1], y0));
            xb = at(std::min(q[1], y1));
        }
        lo = std::min({lo, xa, xb});
        hi = std::max({hi, xa, xb});
    }
    if (lo > hi) {
        return std::nullopt;
    }
    const auto a = static_cast<std::int64_t>(std::floor(lo));
    return std::make_pair(a, std::max(static_cast<std::int64_t>(std::ceil(hi)) + reach_texels, a + 1));
}

} // namespace

Filter filterFromGl(std::optional<std::uint64_t> mag, std::optional<std::uint64_t> min) {
    // NEAREST, NEAREST_MIPMAP_NEAREST and NEAREST_MIPMAP_LINEAR sample each level nearest.
    const bool min_nearest = min && (*min == 9728 || *min == 9984 || *min == 9986);
    return mag == std::optional<std::uint64_t>(9728) && min_nearest ? Filter::nearest : Filter::linear;
}

std::optional<Wrap> wrapFromGl(std::uint64_t mode) {
    switch (mode) {
    case 10497:
        return Wrap::repeat;
    case 33071:
        return Wrap::clamp;
    case 33648:
        return Wrap::mirror;
    default:
        return std::nullopt;
    }
}

Coverage::Coverage(common::Workspace& workspace, const std::string& name, std::uint32_t width, std::uint32_t height)
    : width_(width), height_(height), stride_((std::size_t{width} + 7) / 8) {
    map_ = workspace.scratch(name, std::uint64_t{stride_} * height);
}

Coverage::Result Coverage::triangle(const std::array<Point, 3>& uv, const std::array<Wrap, 2>& wrap, Filter filter) {
    if (width_ == 0 || height_ == 0) {
        return Result::drawn;
    }
    const std::int64_t r = reach(filter);
    const double shift = static_cast<double>(r) / 2.0;
    std::array<Point, 3> tri{};
    for (std::size_t i = 0; i < 3; ++i) {
        tri[i] = {uv[i][0] * static_cast<double>(width_) - shift, uv[i][1] * static_cast<double>(height_) - shift};
        for (const double c : tri[i]) {
            if (std::isnan(c) || std::abs(c) >= limit) {
                return Result::invalid;
            }
        }
    }
    const double ymin = std::min({tri[0][1], tri[1][1], tri[2][1]});
    const double ymax = std::max({tri[0][1], tri[1][1], tri[2][1]});
    const auto r0 = static_cast<std::int64_t>(std::floor(ymin));
    const std::int64_t r1 = std::max(static_cast<std::int64_t>(std::ceil(ymax)) - 1 + r, r0);
    // Row t reads the triangle's part with t - reach <= y <= t + 1.
    const auto slab = [&](std::int64_t t) {
        return std::make_pair(std::max(ymin, static_cast<double>(t - r)), std::min(ymax, static_cast<double>(t + 1)));
    };
    if (wrap[1] == Wrap::clamp) {
        // Rows outside the texture all sample its edge rows.
        const std::int64_t first = std::clamp<std::int64_t>(r0, 0, height_ - 1);
        const std::int64_t last = std::clamp<std::int64_t>(r1, 0, height_ - 1);
        for (std::int64_t t = first; t <= last; ++t) {
            auto [y0, y1] = slab(t);
            if (t == 0) {
                y0 = ymin;
            }
            if (t == height_ - 1) {
                y1 = ymax;
            }
            row(t, span(tri, y0, y1, r), wrap[0]);
        }
    } else {
        if (r1 - r0 >= max_repeats * height_) {
            return Result::too_many_repeats;
        }
        for (std::int64_t t = r0; t <= r1; ++t) {
            const auto [y0, y1] = slab(t);
            row(wrapIndex(wrap[1], t, height_), span(tri, y0, y1, r), wrap[0]);
        }
    }
    return Result::drawn;
}

void Coverage::row(std::int64_t row_index, Span columns, Wrap wrap) {
    if (!columns) {
        return;
    }
    std::uint8_t* line = map_.data() + static_cast<std::size_t>(row_index) * stride_;
    wrapRanges(wrap, columns->first, columns->second, width_, [&](std::int64_t a64, std::int64_t b64) {
        const auto a = static_cast<std::size_t>(a64);
        const auto b = static_cast<std::size_t>(b64);
        const std::size_t first = a / 8;
        const std::size_t last = (b - 1) / 8;
        const auto low = static_cast<std::uint8_t>(0xFFu << (a % 8));
        const auto high = static_cast<std::uint8_t>(0xFFu >> (7 - (b - 1) % 8));
        if (first == last) {
            line[first] |= static_cast<std::uint8_t>(low & high);
        } else {
            line[first] |= low;
            std::memset(line + first + 1, 0xFF, last - first - 1);
            line[last] |= high;
        }
    });
}

std::uint64_t Coverage::count() const {
    std::uint64_t total = 0;
    const std::uint8_t* data = map_.data();
    for (std::size_t i = 0; i < map_.size(); ++i) {
        total += static_cast<std::uint64_t>(__builtin_popcount(data[i]));
    }
    return total;
}

bool Coverage::test(std::uint32_t column, std::uint32_t row_index) const {
    return ((map_.data()[std::size_t{row_index} * stride_ + column / 8] >> (column % 8)) & 1) != 0;
}

} // namespace meshtinker::analysis
