#include "meshtinker/common/diagnostics.hpp"
#include "meshtinker/gltf/document.hpp"
#include "meshtinker/gltf/edit.hpp"
#include "meshtinker/gltf/geometry.hpp"
#include "meshtinker/gltf/layout.hpp"
#include "meshtinker/gltf/writer.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <meshoptimizer.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>

namespace fs = std::filesystem;
using namespace meshtinker;
using gltf::Document;
using gltf::Json;

namespace {

const gltf::ReadOptions trusted{true};

std::map<std::string, std::string> filesIn(const fs::path& directory) {
    std::map<std::string, std::string> files;
    for (const auto& entry : fs::directory_iterator(directory)) {
        std::ifstream in(entry.path(), std::ios::binary);
        files[entry.path().filename().u8string()] =
            std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    return files;
}

// What a command writes: the layout of its normalized input, edited by `edit`.
template <typename Edit>
void writeEdited(const fs::path& input, const fs::path& directory, const std::string& stem, Edit edit) {
    common::Diagnostics diagnostics;
    Document document = gltf::readNormalized(input, trusted, diagnostics);
    gltf::Layout layout = gltf::normalizedLayout(document, gltf::selectScene(document), diagnostics);
    edit(document, layout);
    gltf::prune(layout);
    common::Outputs outputs;
    gltf::writeSeparate(document, layout, directory, stem, outputs);
    outputs.commit();
}

void importTo(const fs::path& input, const fs::path& directory, const std::string& stem) {
    common::Diagnostics diagnostics;
    const Document source = Document::read(input, trusted);
    const gltf::Layout layout = gltf::normalizedLayout(source, gltf::selectScene(source), diagnostics);
    common::Outputs outputs;
    gltf::writeSeparate(source, layout, directory, stem, outputs);
    outputs.commit();
}

std::vector<float> readFloats(const cgltf_accessor& accessor) {
    std::vector<float> values(cgltf_accessor_unpack_floats(&accessor, nullptr, 0));
    cgltf_accessor_unpack_floats(&accessor, values.data(), values.size());
    return values;
}

std::vector<std::uint32_t> readIndices(const cgltf_accessor& accessor) {
    std::vector<std::uint32_t> values(accessor.count);
    for (std::size_t i = 0; i < accessor.count; ++i) {
        values[i] = static_cast<std::uint32_t>(cgltf_accessor_read_index(&accessor, i));
    }
    return values;
}

bool usesExtension(const Json& json, const char* name) {
    const auto used = json.find("extensionsUsed");
    return used != json.end() && std::find(used->begin(), used->end(), Json(name)) != used->end();
}

template <typename T>
common::ByteView viewOf(const std::vector<T>& values) {
    return {reinterpret_cast<const std::uint8_t*>(values.data()), values.size() * sizeof(T)};
}

// Replaces mesh 0, primitive 0 with plain float positions and uint32
// indices, read from the input as they decode.
struct PlainCopy {
    std::vector<float> positions;
    std::vector<std::uint32_t> indices;

    void operator()(Document& document, gltf::Layout& layout) {
        common::Workspace workspace(test::workDirectory("edit-workspace"), false);
        gltf::GeometryReader reader(document, workspace);
        const cgltf_primitive& primitive = document.data().meshes[0].primitives[0];
        CHECK(reader.preparePrimitive(0, 0) == gltf::Readability::readable);
        const cgltf_accessor* position = gltf::findAttribute(primitive, cgltf_attribute_type_position, 0);
        CHECK(reader.prepare(*position) == gltf::Readability::readable);
        CHECK(reader.prepare(*primitive.indices) == gltf::Readability::readable);
        positions = readFloats(*position);
        indices = readIndices(*primitive.indices);
        const std::size_t vertex_view = gltf::addView(
            layout, Json{{"byteLength", positions.size() * 4}, {"target", 34962}}, viewOf(positions));
        const std::size_t index_view =
            gltf::addView(layout, Json{{"byteLength", indices.size() * 4}, {"target", 34963}}, viewOf(indices));
        gltf::PrimitiveGeometry geometry;
        geometry.attributes["POSITION"] = gltf::addAccessor(
            layout, Json{{"bufferView", vertex_view}, {"componentType", 5126}, {"count", positions.size() / 3},
                         {"type", "VEC3"}, {"min", Json::array({-1e9, -1e9, -1e9})}, {"max", Json::array({1e9, 1e9, 1e9})}});
        geometry.indices = gltf::addAccessor(
            layout, Json{{"bufferView", index_view}, {"componentType", 5125}, {"count", indices.size()}, {"type", "SCALAR"}});
        gltf::replaceGeometry(layout, 0, 0, geometry);
    }
};

} // namespace

// A command that edits nothing writes what `import` writes, byte for byte.
TEST_CASE(unedited_layout_writes_the_import) {
    for (const test::Fixture& f : test::fixtures()) {
        if (f.name == "refs-absolute" || f.name == "invalid") {
            continue;
        }
        const fs::path imported = test::workDirectory("edit/unedited/" + f.name + "/import");
        const fs::path rewritten = test::workDirectory("edit/unedited/" + f.name + "/rewritten");
        importTo(f.model, imported, f.name);
        writeEdited(f.model, rewritten, f.name, [](Document&, gltf::Layout&) {});
        CHECK(filesIn(imported) == filesIn(rewritten));
        if (test::failureCount() > 0) {
            std::fprintf(stderr, "  in fixture %s\n", f.name.c_str());
            return;
        }
    }
}

// Replacing a Draco primitive drops the extension and the accessors only it used.
TEST_CASE(replaced_draco_primitive_reads_back_plain) {
    const test::Fixture& f = test::fixture("draco");
    const fs::path out = test::workDirectory("edit/draco");
    PlainCopy copy;
    writeEdited(f.model, out, "draco", std::ref(copy));
    const Document written = Document::read(out / "draco.gltf");
    const Json& json = written.json();
    CHECK(!usesExtension(json, "KHR_draco_mesh_compression"));
    CHECK(!json.contains("extensionsRequired") || json["extensionsRequired"].empty());
    const cgltf_primitive& primitive = written.data().meshes[0].primitives[0];
    CHECK(!primitive.has_draco_mesh_compression);
    CHECK_EQ(written.data().accessors_count, std::size_t{2});
    CHECK(readFloats(*gltf::findAttribute(primitive, cgltf_attribute_type_position, 0)) == copy.positions);
    CHECK(readIndices(*primitive.indices) == copy.indices);
    // Only the new views remain, in primitive order: indices first.
    CHECK_EQ(json["bufferViews"].size(), std::size_t{2});
    CHECK_EQ(json["bufferViews"][1]["byteOffset"].get<std::uint64_t>(), std::uint64_t{0});
    CHECK(cgltf_validate(const_cast<cgltf_data*>(&written.data())) == cgltf_result_success);
}

// Accessors other primitives still use survive the edit, renumbered.
TEST_CASE(prune_keeps_what_others_use) {
    const test::Fixture& f = test::fixture("shared");
    const fs::path out = test::workDirectory("edit/shared");
    PlainCopy copy;
    writeEdited(f.model, out, "shared", std::ref(copy));
    const Document written = Document::read(out / "shared.gltf");
    CHECK(cgltf_validate(const_cast<cgltf_data*>(&written.data())) == cgltf_result_success);
    const cgltf_primitive& primitive = written.data().meshes[0].primitives[0];
    CHECK(readFloats(*gltf::findAttribute(primitive, cgltf_attribute_type_position, 0)) == copy.positions);
    for (std::size_t m = 0; m < written.data().meshes_count; ++m) {
        for (std::size_t p = 0; p < written.data().meshes[m].primitives_count; ++p) {
            const cgltf_primitive& other = written.data().meshes[m].primitives[p];
            CHECK(gltf::findAttribute(other, cgltf_attribute_type_position, 0) != nullptr);
        }
    }
}

// An added view with a meshopt extension goes in the .bin compressed, and
// its own range in a new fallback buffer.
TEST_CASE(added_meshopt_view_decodes) {
    const test::Fixture& f = test::fixture("triangle");
    const fs::path out = test::workDirectory("edit/meshopt");
    std::vector<float> positions;
    std::vector<std::uint8_t> encoded;
    writeEdited(f.model, out, "triangle", [&](Document& document, gltf::Layout& layout) {
        const cgltf_primitive& primitive = document.data().meshes[0].primitives[0];
        positions = readFloats(*gltf::findAttribute(primitive, cgltf_attribute_type_position, 0));
        const std::size_t count = positions.size() / 3;
        encoded.resize(meshopt_encodeVertexBufferBound(count, 12));
        encoded.resize(meshopt_encodeVertexBufferLevel(encoded.data(), encoded.size(), positions.data(), count, 12, 2, 0));
        const Json extension{{"byteLength", encoded.size()}, {"byteStride", 12}, {"count", count}, {"mode", "ATTRIBUTES"}};
        const std::size_t view = gltf::addView(
            layout,
            Json{{"byteLength", positions.size() * 4}, {"byteStride", 12}, {"target", 34962},
                 {"extensions", {{"EXT_meshopt_compression", extension}}}},
            viewOf(encoded));
        Json& accessor = layout.json["accessors"][static_cast<std::size_t>(primitive.attributes[0].data - document.data().accessors)];
        accessor["bufferView"] = view;
        accessor.erase("byteOffset");
        gltf::useExtension(layout.json, "EXT_meshopt_compression", true);
    });
    Document written = Document::read(out / "triangle.gltf");
    const Json& json = written.json();
    CHECK(usesExtension(json, "EXT_meshopt_compression"));
    const Json& fallback = json["buffers"].back();
    CHECK(fallback["extensions"]["EXT_meshopt_compression"]["fallback"].get<bool>());
    CHECK_EQ(fallback["byteLength"].get<std::uint64_t>(), std::uint64_t{positions.size() * 4});
    common::Workspace workspace(test::workDirectory("edit/meshopt-workspace"), false);
    gltf::GeometryReader reader(written, workspace);
    const cgltf_accessor& position =
        *gltf::findAttribute(written.data().meshes[0].primitives[0], cgltf_attribute_type_position, 0);
    CHECK(reader.prepare(position) == gltf::Readability::readable);
    CHECK(readFloats(position) == positions);
}

// An image the command wrote itself replaces the texture's source: the writer
// points at its file, and prune drops the image it replaced.
TEST_CASE(added_image_replaces_a_source) {
    const test::Fixture& f = test::fixture("quantized");
    const fs::path out = test::workDirectory("edit/added-image");
    common::Diagnostics diagnostics;
    Document document = gltf::readNormalized(f.model, trusted, diagnostics);
    gltf::Layout layout = gltf::normalizedLayout(document, gltf::selectScene(document), diagnostics);
    gltf::ImageNames names(document, "quantized");
    CHECK_EQ(names.reserve("grid.png"), std::string("grid_1.png"));
    CHECK_EQ(names.reserve("GRID.ktx2"), std::string("GRID.ktx2"));
    CHECK_EQ(names.reserve("grid.ktx2"), std::string("grid_1.ktx2"));
    CHECK_EQ(names.reserve("quantized.bin"), std::string("quantized_1.bin"));

    common::Outputs outputs;
    const std::string bytes = "not really KTX2";
    common::OutputFile file = outputs.create(out / "grid_1.ktx2");
    file.write({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
    file.close();
    const std::size_t image = gltf::addImage(layout, Json{{"mimeType", "image/ktx2"}}, "grid_1.ktx2", bytes.size());
    Json& texture = layout.json["textures"][0];
    texture.erase("source");
    texture["extensions"]["KHR_texture_basisu"]["source"] = image;
    gltf::prune(layout);
    const std::vector<gltf::WrittenFile> files = gltf::writeSeparate(document, layout, out, "quantized", outputs);
    outputs.commit();

    const std::map<std::string, std::string> written = filesIn(out);
    CHECK(written.count("grid.png") == 0);
    CHECK_EQ(written.at("grid_1.ktx2"), bytes);
    const Json json = Json::parse(written.at("quantized.gltf"));
    CHECK_EQ(json["images"].size(), std::size_t{1});
    CHECK_EQ(json["images"][0]["uri"].get<std::string>(), std::string("grid_1.ktx2"));
    CHECK_EQ(json["textures"][0]["extensions"]["KHR_texture_basisu"]["source"].get<int>(), 0);
    const auto listed = std::find_if(files.begin(), files.end(), [](const gltf::WrittenFile& w) {
        return w.role == gltf::FileRole::image;
    });
    CHECK(listed != files.end() && listed->name == "grid_1.ktx2" && listed->size == bytes.size());
}
