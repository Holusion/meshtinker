#pragma once

#include "meshtinker/common/large_array.hpp"
#include "meshtinker/gltf/document.hpp"
#include "meshtinker/gltf/primitive_data.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace meshtinker::compress {

using gltf::Json;

// Bits per quantized value, after glTF-Transform's `quantize` defaults with
// `meshopt({level: "high"})`, the preset the eCorpus compression guide picks:
// 8-bit octahedral normals instead of medium's 10 bits, the rest as medium.
constexpr int position_bits = 14;
constexpr int normal_bits = 8;
constexpr int texcoord_bits = 12;
constexpr int color_bits = 8;
constexpr int weight_bits = 8;

// A position grid: a position's grid index is (p − center) / step, rounded,
// stored as a normalized signed short, which a reader divides by 32767. The
// node transform multiplies it back by nodeScale() and translates it by the
// center. One step for all axes, so normals need no correction.
//
// The index stays unshifted, within ±(2^13 − 1): shifting it up to the
// short's full range, as glTF-Transform does with a second rounding, adds
// no precision and costs 4.5 to 13 % of the compressed geometry. Normalized
// storage keeps the node's scale near the model's size, where plain integers
// would need the step itself (about 6e-5 for a model a unit across).
struct PositionGrid {
    std::array<double, 3> center{};
    double step = 1;

    // step × 32767: what turns the normalized value back into the distance
    // from the center. Decoding is exactly center + step × index.
    double nodeScale() const { return step * 32767.0; }
};

// The grid that spans `min`..`max` with `position_bits` bits on the longest axis.
PositionGrid gridFor(const std::array<double, 3>& min, const std::array<double, 3>& max);

// The source accessor's storage, for values kept as they were stored.
struct SourceType {
    int component_type = 5126;
    bool normalized = false;
};
SourceType sourceType(const cgltf_accessor& accessor);

// A buffer view ready for the writer: its compressed bytes and its JSON,
// with the accessor that reads it.
struct EncodedView {
    common::LargeArray<std::uint8_t> bytes;
    std::size_t size = 0;
    Json view;
    Json accessor;
};

// Quantizes `attribute` (`count` vertices) for its name and encodes it with
// meshoptimizer's vertex codec, version 0 as EXT_meshopt_compression requires.
// POSITION uses `grid` when it has one, and is kept float otherwise.
EncodedView encodeAttribute(const gltf::AttributeData& attribute, const SourceType& source, bool morph_target,
                            const std::optional<PositionGrid>& grid, common::Workspace& workspace);

// Encodes indices, unsigned shorts up to 65,535 vertices: as a triangle list
// (meshopt's TRIANGLES mode) when `triangles`, else as a sequence (INDICES).
EncodedView encodeIndices(const common::LargeArray<std::uint32_t>& indices, std::size_t vertex_count, bool triangles,
                          common::Workspace& workspace);

} // namespace meshtinker::compress
