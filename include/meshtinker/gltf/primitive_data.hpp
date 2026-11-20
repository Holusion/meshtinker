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
    // The accessor it was read from, whose name, extras and extensions the
    // rewritten accessor keeps (copyAccessorProperties).
    std::size_t source_accessor = static_cast<std::size_t>(-1);
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
    // The index accessor read, if any.
    std::size_t indices_source = static_cast<std::size_t>(-1);
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

// The vertices `indices` use, in the order they first appear
// (meshopt_optimizeVertexFetchRemap), with every attribute and morph target
// remapped, and `indices` renumbered to match.
TriangleData compactVertices(const TriangleData& source, common::LargeArray<std::uint32_t>&& indices,
                             common::Workspace& workspace);

// Reads every element of `accessor` as floats into `out`, named `name`. The
// accessor must be readable (GeometryReader::prepare). False if it has more
// than 4 components.
bool readAccessor(const cgltf_data& data, const cgltf_accessor& accessor, const std::string& name,
                  common::Workspace& workspace, AttributeData& out);

// Adds `data` to `layout` as plain accessors, in the profile's form for
// rewritten geometry (docs/normalized-profile.md, "Commands that rewrite
// geometry"): float32 attributes, JOINTS as unsigned bytes or shorts as in the
// source, POSITION with exact min and max, and indices as unsigned shorts up
// to 65,535 vertices, unsigned ints beyond. `data` must outlive the write:
// the views read its arrays, and its indices are converted in place.
PrimitiveGeometry addPlainGeometry(Layout& layout, TriangleData& data);

} // namespace meshtinker::gltf
