#pragma once

#include "meshtinker/common/large_array.hpp"
#include "meshtinker/common/workspace.hpp"
#include "meshtinker/gltf/document.hpp"
#include "meshtinker/gltf/edit.hpp"
#include "meshtinker/gltf/geometry.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace meshtinker::gltf {

// One vertex attribute read as floats, `components` per vertex.
struct AttributeData {
    std::string name;
    std::size_t components = 0;
    common::LargeArray<float> values;
    // What the source stored, for attributes written back as integers (JOINTS).
    int source_component_type = 0;
};

// A triangle primitive read into working arrays (common::LargeArray), so a
// command can rewrite it. Every attribute is read as floats, through cgltf, so
// normalized and quantized data come out dequantized.
struct TriangleData {
    std::size_t vertex_count = 0;
    // Three per triangle. Strips and fans become lists, with the winding of
    // every triangle kept, and degenerate triangles dropped; a primitive
    // without indices gets 0..n−1.
    common::LargeArray<std::uint32_t> indices;
    // In the source's attribute order. POSITION is always there.
    std::vector<AttributeData> attributes;
    // Morph targets, each with the attributes it displaces.
    std::vector<std::vector<AttributeData>> targets;

    const AttributeData& position() const;
};

// Why a primitive can't be rewritten as triangles.
enum class TriangleReadStatus : std::uint8_t {
    ok,
    // Points or lines.
    not_triangles,
    // No POSITION, or more than 2^32 − 1 vertices or corners.
    unsupported,
    // GeometryReader couldn't make an accessor readable.
    unreadable,
};

// Reads primitive `primitive` of mesh `mesh`, decoding it with `reader` first.
TriangleReadStatus readTriangles(Document& document, GeometryReader& reader, common::Workspace& workspace,
                                 std::size_t mesh, std::size_t primitive, TriangleData& data);

// Adds `data` to `layout` as plain accessors, in the profile's form for
// rewritten geometry (docs/normalized-profile.md, "Commands that rewrite
// geometry"): float32 attributes, JOINTS as unsigned bytes or shorts as in the
// source, POSITION with exact min and max, and indices as unsigned shorts up
// to 65,535 vertices, unsigned ints beyond. `data` must outlive the write:
// the views read its arrays, and its indices are converted in place.
PrimitiveGeometry addPlainGeometry(Layout& layout, TriangleData& data);

} // namespace meshtinker::gltf
