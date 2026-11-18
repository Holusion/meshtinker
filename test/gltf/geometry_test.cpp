#include "meshtinker/gltf/geometry.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;
using namespace meshtinker;

namespace {

const cgltf_accessor* position(const cgltf_primitive& primitive) {
    for (std::size_t a = 0; a < primitive.attributes_count; ++a) {
        if (primitive.attributes[a].type == cgltf_attribute_type_position) {
            return primitive.attributes[a].data;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE(decodes_draco_and_restores_accessors) {
    gltf::Document document = gltf::Document::read(test::fixture("draco").model);
    const cgltf_primitive& primitive = document.data().meshes[0].primitives[0];
    const cgltf_buffer_view* before = position(primitive)->buffer_view;
    {
        common::Workspace workspace(test::workDirectory("geometry"), false);
        gltf::GeometryReader reader(document, workspace);
        CHECK(reader.preparePrimitive(0, 0) == gltf::Readability::readable);
        const cgltf_accessor& positions = *position(primitive);
        CHECK(reader.prepare(positions) == gltf::Readability::readable);
        CHECK(reader.prepare(*primitive.indices) == gltf::Readability::readable);
        // The 8×8 grid spans [0, 1] in x and y.
        float low[3] = {1e9f, 1e9f, 1e9f};
        float high[3] = {-1e9f, -1e9f, -1e9f};
        for (std::size_t v = 0; v < positions.count; ++v) {
            float p[3];
            CHECK(cgltf_accessor_read_float(&positions, v, p, 3));
            for (int i = 0; i < 3; ++i) {
                low[i] = std::min(low[i], p[i]);
                high[i] = std::max(high[i], p[i]);
            }
        }
        CHECK(low[0] > -1e-3f && low[1] > -1e-3f && high[0] < 1.001f && high[0] > 0.999f && high[1] > 0.999f);
        for (std::size_t i = 0; i < primitive.indices->count; ++i) {
            CHECK(cgltf_accessor_read_index(primitive.indices, i) < positions.count);
        }
    }
    // The reader is gone: the accessor reads its own data again.
    CHECK(position(primitive)->buffer_view == before);
}

TEST_CASE(corrupted_draco_data_is_reported) {
    // A copy of the Draco fixture with the compressed bytes zeroed.
    const fs::path directory = test::workDirectory("geometry-corrupt");
    const fs::path copy = directory / "draco.glb";
    fs::copy_file(test::fixture("draco").model, copy);
    {
        gltf::Document original = gltf::Document::read(copy);
        const cgltf_buffer_view* view = original.data().meshes[0].primitives[0].draco_mesh_compression.buffer_view;
        const std::size_t offset = static_cast<std::size_t>(
            static_cast<const std::uint8_t*>(view->buffer->data) + view->offset - original.buffer(0).data);
        const std::size_t size = view->size;
        std::fstream file(copy, std::ios::in | std::ios::out | std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        // The BIN chunk's data starts after the GLB header, the JSON chunk and the BIN chunk header.
        std::size_t json_length = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            json_length |= std::size_t{static_cast<std::uint8_t>(bytes[12 + i])} << (8 * i);
        }
        const std::size_t start = 20 + json_length + 8 + offset;
        std::fill(bytes.begin() + static_cast<std::ptrdiff_t>(start), bytes.begin() + static_cast<std::ptrdiff_t>(start + size), 0);
        file.seekp(0);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    gltf::Document document = gltf::Document::read(copy);
    common::Workspace workspace(directory, false);
    gltf::GeometryReader reader(document, workspace);
    CHECK(reader.preparePrimitive(0, 0) == gltf::Readability::draco_failed);
}
