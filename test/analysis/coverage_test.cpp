// Ported from objtools' src/inspect/coverage.rs tests.
#include "meshtinker/analysis/coverage.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <cmath>
#include <limits>
#include <memory>

using namespace meshtinker;
using analysis::Coverage;
using analysis::Filter;
using analysis::Wrap;
using Tri = std::array<std::array<double, 2>, 3>;

namespace {

constexpr std::array<Wrap, 2> repeat{Wrap::repeat, Wrap::repeat};
constexpr std::array<Wrap, 2> clamp{Wrap::clamp, Wrap::clamp};

struct Fixture {
    common::Workspace workspace;
    Coverage coverage;
    Fixture(std::uint32_t width, std::uint32_t height)
        : workspace(test::workDirectory("coverage"), false), coverage(workspace, "map", width, height) {}
};

// Row byte `index` of an 8-texel-wide map: bit i is column i.
std::uint8_t rowBits(const Coverage& c, std::uint32_t row) {
    std::uint8_t bits = 0;
    for (std::uint32_t column = 0; column < 8; ++column) {
        bits = static_cast<std::uint8_t>(bits | (c.test(column, row) ? 1u << column : 0u));
    }
    return bits;
}

// Separating-axis test of a triangle against the open square [c - m, c + 1 + m] x
// [r - m, r + 1 + m]: the texel itself (m = 0) or its bilinear footprint (m = 0.5).
bool overlaps(const Tri& tri, double c, double r, double m) {
    const double c0 = c - m, c1 = c + 1 + m, r0 = r - m, r1 = r + 1 + m;
    const Tri::value_type square[4] = {{c0, r0}, {c1, r0}, {c1, r1}, {c0, r1}};
    std::vector<std::array<double, 2>> axes{{1, 0}, {0, 1}};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto& p = tri[i];
        const auto& q = tri[(i + 1) % 3];
        axes.push_back({q[1] - p[1], p[0] - q[0]});
    }
    for (const auto& a : axes) {
        double t0 = std::numeric_limits<double>::infinity(), t1 = -t0, s0 = t0, s1 = -t0;
        for (const auto& p : tri) {
            const double d = p[0] * a[0] + p[1] * a[1];
            t0 = std::min(t0, d);
            t1 = std::max(t1, d);
        }
        for (const auto& p : square) {
            const double d = p[0] * a[0] + p[1] * a[1];
            s0 = std::min(s0, d);
            s1 = std::max(s1, d);
        }
        if (!(t0 < s1 && s0 < t1)) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE(full_square_and_overlap_count_once) {
    Fixture f(10, 6);
    for (int i = 0; i < 2; ++i) {
        f.coverage.triangle(Tri{{{0, 0}, {1, 0}, {1, 1}}}, repeat, Filter::nearest);
        f.coverage.triangle(Tri{{{0, 0}, {1, 1}, {0, 1}}}, repeat, Filter::nearest);
    }
    CHECK_EQ(f.coverage.count(), 60u);
}

TEST_CASE(texel_aligned_half_counts_diagonal_texels_once) {
    Fixture f(4, 4);
    f.coverage.triangle(Tri{{{0, 0}, {1, 0}, {1, 1}}}, repeat, Filter::nearest);
    // 6 texels strictly below the diagonal, plus the 4 it crosses.
    CHECK_EQ(f.coverage.count(), 10u);
}

TEST_CASE(tiny_triangle_uses_one_texel_and_edges_do_not_leak) {
    Fixture f(8, 8);
    f.coverage.triangle(Tri{{{0.51, 0.51}, {0.52, 0.51}, {0.51, 0.52}}}, repeat, Filter::nearest);
    CHECK_EQ(f.coverage.count(), 1u);
    // Exactly on texel boundaries: covers texels 2 and 3 of row 2 only.
    f.coverage.triangle(Tri{{{0.25, 0.25}, {0.5, 0.25}, {0.25, 0.375}}}, repeat, Filter::nearest);
    CHECK_EQ(f.coverage.count(), 3u);
}

TEST_CASE(wrap_modes) {
    // A strip one texel high, from u = -0.25 to 0.25 on an 8-texel-wide texture.
    const auto strip = [](Coverage& c, Wrap wrap) {
        c.triangle(Tri{{{-0.25, 0}, {0.25, 0}, {0.25, 0.125}}}, {wrap, Wrap::repeat}, Filter::nearest);
        c.triangle(Tri{{{-0.25, 0}, {0.25, 0.125}, {-0.25, 0.125}}}, {wrap, Wrap::repeat}, Filter::nearest);
    };
    {
        Fixture f(8, 8);
        strip(f.coverage, Wrap::repeat);
        CHECK_EQ(int{rowBits(f.coverage, 0)}, 0b1100'0011);
    }
    {
        Fixture f(8, 8);
        strip(f.coverage, Wrap::mirror);
        CHECK_EQ(int{rowBits(f.coverage, 0)}, 0b0000'0011);
    }
    {
        Fixture f(8, 8);
        strip(f.coverage, Wrap::clamp);
        CHECK_EQ(int{rowBits(f.coverage, 0)}, 0b0000'0011);
    }
    {
        // Clamped rows: a triangle far below the texture lands on row 0.
        Fixture f(8, 8);
        f.coverage.triangle(Tri{{{0, -3}, {0.1, -3}, {0, -2}}}, clamp, Filter::nearest);
        CHECK_EQ(f.coverage.count(), 1u);
        CHECK_EQ(int{rowBits(f.coverage, 0)}, 1);
    }
}

TEST_CASE(linear_filtering_reads_the_2x2_footprint) {
    {
        // A tiny triangle at texel coordinates ~(4.1, 4.1): bilinear sampling there
        // blends texels 3 and 4 on each axis, whose centers 3.5 and 4.5 are within one texel.
        Fixture f(8, 8);
        f.coverage.triangle(Tri{{{0.51, 0.51}, {0.52, 0.51}, {0.51, 0.52}}}, repeat, Filter::linear);
        CHECK_EQ(f.coverage.count(), 4u);
        CHECK_EQ(int{rowBits(f.coverage, 3)}, 0b0001'1000);
        CHECK_EQ(int{rowBits(f.coverage, 4)}, 0b0001'1000);
    }
    {
        // Texel-aligned left half: linear reads one more column on each side, and with
        // repeat wrapping the column left of u = 0 is the texture's last one.
        Fixture f(8, 8);
        f.coverage.triangle(Tri{{{0, 0}, {0.5, 0}, {0.5, 1}}}, repeat, Filter::linear);
        f.coverage.triangle(Tri{{{0, 0}, {0.5, 1}, {0, 1}}}, repeat, Filter::linear);
        for (std::uint32_t row = 0; row < 8; ++row) {
            CHECK_EQ(int{rowBits(f.coverage, row)}, 0b1001'1111);
        }
    }
    {
        Fixture f(8, 8);
        f.coverage.triangle(Tri{{{0, 0}, {0.5, 0}, {0.5, 1}}}, clamp, Filter::linear);
        f.coverage.triangle(Tri{{{0, 0}, {0.5, 1}, {0, 1}}}, clamp, Filter::linear);
        for (std::uint32_t row = 0; row < 8; ++row) {
            CHECK_EQ(int{rowBits(f.coverage, row)}, 0b0001'1111);
        }
    }
}

TEST_CASE(filters_follow_gltf_samplers) {
    using O = std::optional<std::uint64_t>;
    CHECK(analysis::filterFromGl(O(), O()) == Filter::linear);
    CHECK(analysis::filterFromGl(O(9728), O()) == Filter::linear);
    CHECK(analysis::filterFromGl(O(9728), O(9729)) == Filter::linear);
    CHECK(analysis::filterFromGl(O(9728), O(9985)) == Filter::linear);
    CHECK(analysis::filterFromGl(O(9728), O(9986)) == Filter::nearest);
    CHECK(analysis::filterFromGl(O(9729), O(9728)) == Filter::linear);
}

TEST_CASE(matches_brute_force_overlap) {
    std::uint64_t seed = 0x2545'f491'4f6c'dd1dULL;
    const auto random = [&seed] {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        return static_cast<double>(seed >> 11) / static_cast<double>(1ULL << 53);
    };
    const std::uint32_t w = 13, h = 11;
    int tested = 0;
    common::Workspace workspace(test::workDirectory("coverage-brute"), false);
    for (int index = 0; index < 8000; ++index) {
        // Mostly small triangles, as in photogrammetry atlases, plus some large ones.
        const double scale = index % 4 == 0 ? 1.0 : 0.15;
        const double u = random(), v = random();
        Tri tri{};
        for (auto& p : tri) {
            const double x = u + (random() - 0.5) * scale;
            const double y = v + (random() - 0.5) * scale;
            p = {x, y};
        }
        // Clamping would make degenerate edge-hugging triangles, which the open-square test rejects.
        bool inside = true;
        for (const auto& p : tri) {
            inside = inside && p[0] >= 0 && p[0] <= 1 && p[1] >= 0 && p[1] <= 1;
        }
        if (!inside) {
            continue;
        }
        ++tested;
        for (const auto& [filter, margin] : {std::pair{Filter::nearest, 0.0}, std::pair{Filter::linear, 0.5}}) {
            Coverage c(workspace, "map-" + std::to_string(index) + (filter == Filter::linear ? "l" : "n"), w, h);
            c.triangle(tri, clamp, filter);
            Tri texels{};
            for (std::size_t i = 0; i < 3; ++i) {
                texels[i] = {tri[i][0] * w, tri[i][1] * h};
            }
            for (std::uint32_t r = 0; r < h; ++r) {
                for (std::uint32_t col = 0; col < w; ++col) {
                    if (c.test(col, r) != overlaps(texels, col, r, margin)) {
                        test::fail("case " + std::to_string(index) + ": texel " + std::to_string(col) + "," +
                                       std::to_string(r),
                                   __FILE__, __LINE__);
                        return;
                    }
                }
            }
        }
    }
    CHECK(tested > 5000);
}

TEST_CASE(tiling_triangles) {
    {
        Fixture f(8, 8);
        f.coverage.triangle(Tri{{{0, 0}, {100, 0}, {100, 1}}}, repeat, Filter::nearest);
        CHECK_EQ(f.coverage.count(), 64u);
    }
    {
        Fixture f(8, 8);
        CHECK(f.coverage.triangle(Tri{{{0, 0}, {1, 0}, {1, 65}}}, repeat, Filter::nearest) ==
              Coverage::Result::too_many_repeats);
        CHECK_EQ(f.coverage.count(), 0u);
        CHECK(f.coverage.triangle(Tri{{{std::nan(""), 0}, {1, 0}, {1, 1}}}, repeat, Filter::nearest) ==
              Coverage::Result::invalid);
    }
}
