#pragma once

#include "meshtinker/common/workspace.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace meshtinker::analysis {

// How the GPU reads the base level around a sample point (objtools, coverage.rs).
enum class Filter : std::uint8_t {
    // The texel containing the point.
    nearest,
    // The 2×2 texels around it: every texel whose center is less than one
    // texel away on each axis, since bilinear weights only vanish one full
    // texel from a center.
    linear,
};

// From glTF `magFilter` and `minFilter`. The base level is read without
// blending only when both are nearest; engines default unset filters to linear.
Filter filterFromGl(std::optional<std::uint64_t> mag, std::optional<std::uint64_t> min);

enum class Wrap : std::uint8_t { repeat, clamp, mirror };

// From a glTF wrap mode. Empty for an unknown value.
std::optional<Wrap> wrapFromGl(std::uint64_t mode);

// Texel coverage of UV triangles: one bit per texel in a workspace mapping.
// A texel is used when a GPU sampling any point of a triangle reads it from the
// base level. Texels shared by several triangles count once, and padding no
// sample reaches is never counted. A port of objtools' src/inspect/coverage.rs;
// docs/inspect-report.md links its definition.
class Coverage {
public:
    enum class Result : std::uint8_t {
        drawn,
        // The triangle repeats the texture more than 64 times vertically: its
        // rows would be swept one by one, so it is left out.
        too_many_repeats,
        // A coordinate is not finite, or beyond 2^52 texels.
        invalid,
    };

    Coverage(common::Workspace& workspace, const std::string& name, std::uint32_t width, std::uint32_t height);

    // Marks the texels sampling a UV triangle reads. `uv` is in texture
    // coordinates: [0, 1] covers the texture once.
    Result triangle(const std::array<std::array<double, 2>, 3>& uv, const std::array<Wrap, 2>& wrap, Filter filter);

    // Texels used. Padding bits past each row's width are never set.
    std::uint64_t count() const;

    // The bit of texel (column, row), for tests.
    bool test(std::uint32_t column, std::uint32_t row) const;

private:
    void row(std::int64_t row_index, std::optional<std::pair<std::int64_t, std::int64_t>> columns, Wrap wrap);

    common::ScratchMapping map_;
    std::int64_t width_;
    std::int64_t height_;
    std::size_t stride_;
};

} // namespace meshtinker::analysis
