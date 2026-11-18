#pragma once

#include "meshtinker/common/workspace.hpp"
#include "meshtinker/gltf/document.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <set>
#include <string>
#include <map>
#include <memory>
#include <vector>

namespace meshtinker::gltf {

// Attribute `type` of set `set` (TEXCOORD_<set>, for instance), or null.
const cgltf_accessor* findAttribute(const cgltf_primitive& primitive, cgltf_attribute_type type, int set);

// Corners the primitive draws: its indices, or its vertices without them.
std::uint64_t cornerCount(const cgltf_primitive& primitive);

// Triangles: lists n / 3, strips and fans n − 2, points and lines none.
std::uint64_t triangleCount(cgltf_primitive_type type, std::uint64_t corners);

// The corners of triangle `t`, as positions in the index list (or vertex list
// without indices).
std::array<std::uint64_t, 3> triangleCorners(cgltf_primitive_type type, std::uint64_t t);

// Why an accessor can't be read.
enum class Readability : std::uint8_t {
    readable,
    // A buffer view it uses holds no data, and isn't meshopt-compressed.
    no_data,
    // meshoptimizer rejected the compressed data.
    meshopt_failed,
    // Draco rejected the compressed data, or it doesn't match the accessors.
    draco_failed,
};

// Makes accessor data readable through cgltf's accessor functions
// (cgltf_accessor_read_float, cgltf_accessor_read_index), whatever its
// encoding. This is the one place a decoder plugs in.
//
// - Plain and KHR_mesh_quantization data is read in place from the mappings;
//   cgltf dequantizes normalized values.
// - EXT_meshopt_compression and KHR_meshopt_compression views are decoded with
//   meshoptimizer, filters included, the first time an accessor needs them.
//   Up to 4 MiB goes on the heap; larger views go in workspace scratch files.
// - KHR_draco_mesh_compression primitives are decoded with Draco's decoder, the
//   first time preparePrimitive sees them. Each attribute the extension maps
//   and the indices are converted to their accessor's component type, and
//   the accessors are pointed at the decoded data until the reader is
//   destroyed. Draco decodes a whole primitive on the heap first; the copies
//   follow the same heap or scratch rule as meshopt.
class GeometryReader {
public:
    GeometryReader(Document& document, common::Workspace& workspace);
    GeometryReader(const GeometryReader&) = delete;
    GeometryReader& operator=(const GeometryReader&) = delete;
    GeometryReader(GeometryReader&&) = delete;
    GeometryReader& operator=(GeometryReader&&) = delete;
    ~GeometryReader();

    // Decodes primitive `primitive` of mesh `mesh` if it is Draco-compressed.
    // Call it before preparing the primitive's accessors.
    Readability preparePrimitive(std::size_t mesh, std::size_t primitive);

    // Prepares every buffer view `accessor` reads, sparse ones included.
    Readability prepare(const cgltf_accessor& accessor);

    static bool isDraco(const cgltf_primitive& primitive) { return primitive.has_draco_mesh_compression != 0; }

private:
    Readability prepareView(const cgltf_buffer_view* view);
    Readability decodeDraco(std::size_t mesh, std::size_t primitive);
    // Room for `size` decoded bytes, keyed by `name`: heap up to 4 MiB, else scratch.
    std::uint8_t* allocate(const std::string& name, std::uint64_t size);

    Document& document_;
    common::Workspace& workspace_;
    // Decoded data, by buffer view index: heap or scratch.
    std::map<std::size_t, std::vector<std::uint8_t>> heap_;
    std::map<std::size_t, common::ScratchMapping> scratch_;
    std::map<std::size_t, Readability> status_;
    // Draco: decoded bytes by name, the views made for them, and the accessors
    // they replaced, with what they read before.
    std::map<std::string, std::vector<std::uint8_t>> draco_heap_;
    std::map<std::string, common::ScratchMapping> draco_scratch_;
    std::deque<cgltf_buffer_view> draco_views_;
    std::set<const cgltf_buffer_view*> draco_view_set_;
    std::map<std::size_t, Document::AccessorSource> replaced_;
    std::map<std::pair<std::size_t, std::size_t>, Readability> draco_status_;
};

} // namespace meshtinker::gltf
