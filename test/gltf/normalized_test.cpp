#include "meshtinker/common/diagnostics.hpp"
#include "meshtinker/gltf/document.hpp"
#include "meshtinker/gltf/layout.hpp"
#include "meshtinker/gltf/writer.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <cstring>
#include <fstream>
#include <iterator>
#include <map>

namespace fs = std::filesystem;
using namespace meshtinker;
using gltf::Document;
using gltf::Json;

namespace {

// Every fixture that reads; reference fixtures read with --trusted.
bool readable(const test::Fixture& f) { return f.name != "refs-absolute"; }

const gltf::ReadOptions trusted{true};

bool sameBytes(common::ByteView a, common::ByteView b) {
    return a.size == b.size && (a.size == 0 || std::memcmp(a.data, b.data, a.size) == 0);
}

// The JSON without what places bytes: buffers, view offsets, image sources.
Json withoutStorage(Json json) {
    json.erase("buffers");
    if (json.contains("bufferViews")) {
        for (Json& view : json["bufferViews"]) {
            view.erase("buffer");
            view.erase("byteOffset");
            if (view.contains("extensions")) {
                for (auto& extension : view["extensions"].items()) {
                    if (extension.key().find("meshopt_compression") != std::string::npos) {
                        extension.value().erase("buffer");
                        extension.value().erase("byteOffset");
                    }
                }
            }
        }
    }
    if (json.contains("images")) {
        for (Json& image : json["images"]) {
            image.erase("uri");
            image.erase("bufferView");
        }
    }
    return json;
}

// What `import` writes: the normalized layout, as separate files.
void importTo(const fs::path& input, const fs::path& directory, const std::string& stem) {
    common::Diagnostics diagnostics;
    const Document source = Document::read(input, trusted);
    const gltf::Selection selection = gltf::selectScene(source);
    const gltf::Layout layout = gltf::normalizedLayout(source, selection, diagnostics);
    common::Outputs outputs;
    gltf::writeSeparate(source, layout, directory, stem, outputs);
    outputs.commit();
}

std::map<std::string, std::string> filesIn(const fs::path& directory) {
    std::map<std::string, std::string> files;
    for (const auto& entry : fs::directory_iterator(directory)) {
        std::ifstream in(entry.path(), std::ios::binary);
        files[entry.path().filename().u8string()] =
            std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    return files;
}

void reportFixture(const test::Fixture& f) {
    if (test::failureCount() > 0) {
        std::fprintf(stderr, "  in fixture %s\n", f.name.c_str());
    }
}

} // namespace

// The normalized document has the indices, JSON and data of what `import`
// writes: only buffers and offsets differ.
TEST_CASE(matches_what_import_writes) {
    for (const test::Fixture& f : test::fixtures()) {
        if (!readable(f)) {
            continue;
        }
        const fs::path directory = test::workDirectory("normalized/" + f.name);
        importTo(f.model, directory, f.name);
        const Document imported = Document::read(directory / (f.name + ".gltf"));
        common::Diagnostics diagnostics;
        const Document normalized = gltf::readNormalized(f.model, trusted, diagnostics);

        CHECK(withoutStorage(normalized.json()) == withoutStorage(imported.json()));
        CHECK(normalized.path() == f.model);
        const cgltf_data& a = normalized.data();
        const cgltf_data& b = imported.data();
        CHECK_EQ(a.buffer_views_count, b.buffer_views_count);
        for (std::size_t i = 0; i < a.buffer_views_count && i < b.buffer_views_count; ++i) {
            CHECK(sameBytes(normalized.bufferView(i), imported.bufferView(i)));
        }
        CHECK_EQ(a.images_count, b.images_count);
        for (std::size_t i = 0; i < a.images_count && i < b.images_count; ++i) {
            CHECK(sameBytes(normalized.image(i).bytes, imported.image(i).bytes));
            CHECK_EQ(normalized.image(i).missing, imported.image(i).missing);
        }
        CHECK_EQ(a.accessors_count, b.accessors_count);
        for (std::size_t i = 0; i < a.accessors_count && i < b.accessors_count; ++i) {
            const cgltf_accessor& x = a.accessors[i];
            const cgltf_accessor& y = b.accessors[i];
            if (x.buffer_view == nullptr || x.buffer_view->has_meshopt_compression || x.is_sparse ||
                x.buffer_view->buffer->data == nullptr) {
                continue;
            }
            const std::size_t floats = cgltf_num_components(x.type) * x.count;
            std::vector<float> xs(floats);
            std::vector<float> ys(floats);
            CHECK_EQ(cgltf_accessor_unpack_floats(&x, xs.data(), floats), floats);
            CHECK_EQ(cgltf_accessor_unpack_floats(&y, ys.data(), floats), floats);
            CHECK(xs == ys);
        }
        reportFixture(f);
    }
}

// Importing an imported file writes the same files, byte for byte.
TEST_CASE(import_is_a_fixed_point) {
    for (const test::Fixture& f : test::fixtures()) {
        if (!readable(f)) {
            continue;
        }
        const fs::path once = test::workDirectory("fixed-point/" + f.name + "/once");
        const fs::path twice = test::workDirectory("fixed-point/" + f.name + "/twice");
        importTo(f.model, once, f.name);
        importTo(once / (f.name + ".gltf"), twice, f.name);
        CHECK(filesIn(once) == filesIn(twice));
        reportFixture(f);
    }
}

// The input's buffers are read in place: nothing is copied.
TEST_CASE(reads_the_input_in_place) {
    const test::Fixture& f = test::fixture("textured-glb");
    common::Diagnostics diagnostics;
    const Document source = Document::read(f.model);
    const Document normalized = gltf::readNormalized(f.model, {}, diagnostics);
    CHECK(normalized.container() == gltf::Container::glb);
    CHECK_EQ(normalized.files().size(), source.files().size());
    for (std::size_t i = 0; i < normalized.data().buffers_count; ++i) {
        CHECK(!normalized.json()["buffers"][i].contains("uri"));
    }
    CHECK(normalized.image(0).origin == gltf::ImageOrigin::buffer_view);
    CHECK(!normalized.json()["images"][0].contains("bufferView"));
}
