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

// A mesh's position grid: positions are stored as (p − center) / step,
// rounded, in signed shorts; the node transform scales them back. One step
// for all axes, so normals need no correction.
struct PositionGrid {
    std::array<double, 3> center{};
    double step = 1;
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
