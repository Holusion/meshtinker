#include "meshtinker/common/failure.hpp"
#include "meshtinker/gltf/document.hpp"
#include "meshtinker/gltf/layout.hpp"
#include "meshtinker/gltf/selection.hpp"
#include "meshtinker/gltf/writer.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <cstring>
#include <fstream>

namespace fs = std::filesystem;
using namespace meshtinker;
using gltf::Document;
using gltf::Json;

namespace {

bool sameBytes(common::ByteView a, common::ByteView b) {
    return a.size == b.size && (a.size == 0 || std::memcmp(a.data, b.data, a.size) == 0);
}

// The JSON with what writeSeparate rewrites removed.
Json withoutLayout(Json json) {
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

Document writeAndRead(const test::Fixture& f, const fs::path& directory) {
    const Document source = Document::read(f.model);
    common::Outputs outputs;
    gltf::writeSeparate(source, gltf::identityLayout(source), directory, f.name, outputs);
    outputs.commit();
    return Document::read(directory / (f.name + ".gltf"));
}

} // namespace

TEST_CASE(round_trips_every_fixture) {
    for (const test::Fixture& f : test::fixtures()) {
        if (f.name.rfind("refs-", 0) == 0) {
            continue;
        }
        const fs::path directory = test::workDirectory("writer/" + f.name);
        const Document source = Document::read(f.model);
        const Document output = writeAndRead(f, directory);
        CHECK(output.container() == gltf::Container::gltf);
        CHECK(withoutLayout(source.json()) == withoutLayout(output.json()));
        const cgltf_data& data = source.data();
        CHECK_EQ(output.data().buffer_views_count, data.buffer_views_count);
        for (std::size_t i = 0; i < data.buffer_views_count && i < output.data().buffer_views_count; ++i) {
            if (!source.bufferView(i).empty()) {
                CHECK(sameBytes(source.bufferView(i), output.bufferView(i)));
            }
            const cgltf_buffer_view& view = data.buffer_views[i];
            if (view.has_meshopt_compression) {
                const auto& a = view.meshopt_compression;
                const auto& b = output.data().buffer_views[i].meshopt_compression;
                CHECK(sameBytes(source.buffer(static_cast<std::size_t>(a.buffer - data.buffers)).sub(a.offset, a.size),
                                output.buffer(static_cast<std::size_t>(b.buffer - output.data().buffers)).sub(b.offset, b.size)));
            }
        }
        for (std::size_t i = 0; i < data.images_count; ++i) {
            CHECK(sameBytes(source.image(i).bytes, output.image(i).bytes));
            CHECK(output.image(i).origin == gltf::ImageOrigin::file);
        }
        // Only buffers without data stay apart from the .bin.
        std::size_t with_data = 0;
        for (std::size_t i = 0; i < output.data().buffers_count; ++i) {
            with_data += output.buffer(i).empty() ? 0u : 1u;
        }
        CHECK(with_data <= 1);
        if (test::failureCount() > 0) {
            std::fprintf(stderr, "  in fixture %s\n", f.name.c_str());
        }
    }
}

TEST_CASE(names_images_without_collisions) {
    const Document glb = Document::read(test::fixture("textured-glb").model);
    const auto names = gltf::imageFileNames(glb, "out");
    CHECK_EQ(names.size(), 1u);
    CHECK_EQ(names.front().rfind("out_texture_0.", 0), 0u);

    const Document escaped = Document::read(test::fixture("uri-escaping").model);
    const auto kept = gltf::imageFileNames(escaped, "uri escaping");
    CHECK_EQ(kept[0], std::string("base color \xE2\x99\xBB.jpg"));
    CHECK_EQ(kept[1], std::string("100% #1.png"));
}

TEST_CASE(unchanged_files_are_linked_from_the_parent) {
    // An imported file written again unchanged, as a chain stage that edits
    // nothing: the .bin and the images are links to the input's files.
    common::Diagnostics diagnostics;
    for (const char* name : {"scan", "textured-glb", "meshopt", "draco", "cube"}) {
        const test::Fixture& f = test::fixture(name);
        const fs::path parent = test::workDirectory(std::string("writer-link/") + name + "/parent");
        const fs::path child = test::workDirectory(std::string("writer-link/") + name + "/child");
        {
            const Document source = gltf::readNormalized(f.model, {}, diagnostics);
            common::Outputs outputs;
            gltf::writeSeparate(source, gltf::normalizedLayout(source, gltf::selectScene(source), diagnostics), parent,
                                "model", outputs);
            outputs.commit();
            // Outside a chain, nothing is linked.
            CHECK_EQ(outputs.linkedCount(), std::size_t{0});
        }
        const Document imported = gltf::readNormalized(parent / "model.gltf", {}, diagnostics);
        common::Outputs outputs;
        outputs.linkFrom(parent);
        const std::vector<gltf::WrittenFile> files = gltf::writeSeparate(
            imported, gltf::normalizedLayout(imported, gltf::selectScene(imported), diagnostics), child, "model", outputs);
        outputs.commit();
        CHECK_EQ(outputs.linkedCount(), files.size() - 1);
        for (const gltf::WrittenFile& file : files) {
            CHECK_EQ(fs::file_size(child / file.name), file.size);
            if (file.role != gltf::FileRole::gltf) {
                CHECK(fs::equivalent(child / file.name, parent / file.name));
            }
        }
    }
}

TEST_CASE(existing_outputs_are_kept_and_partial_outputs_removed) {
    const test::Fixture& f = test::fixture("triangle");
    const fs::path directory = test::workDirectory("writer-existing");
    std::ofstream(directory / "triangle.gltf") << "mine";
    const Document source = Document::read(f.model);
    try {
        common::Outputs outputs;
        gltf::writeSeparate(source, gltf::identityLayout(source), directory, "triangle", outputs);
        CHECK(false);
    } catch (const common::Failure& failure) {
        CHECK_EQ(failure.code(), std::string("output-exists"));
        CHECK(failure.exitCode() == common::ExitCode::machine);
    }
    CHECK(!fs::exists(directory / "triangle.bin"));
    std::ifstream kept(directory / "triangle.gltf");
    std::string content;
    kept >> content;
    CHECK_EQ(content, std::string("mine"));
}
