#include "meshtinker/common/failure.hpp"
#include "meshtinker/gltf/document.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

using namespace meshtinker;
using gltf::Document;
using test::fixture;

namespace {

bool isReferenceFixture(const std::string& name) { return name.rfind("refs-", 0) == 0; }

// The Failure code reading `path` throws, or "" if it succeeds.
std::string readFailure(const std::filesystem::path& path, bool trusted = false) {
    try {
        Document::read(path, {trusted});
    } catch (const common::Failure& failure) {
        CHECK(failure.exitCode() == common::ExitCode::input_rejected);
        return failure.code();
    }
    return "";
}

} // namespace

TEST_CASE(reads_every_fixture) {
    for (const test::Fixture& f : test::fixtures()) {
        if (isReferenceFixture(f.name)) {
            continue;
        }
        const Document document = Document::read(f.model);
        CHECK_EQ(document.data().buffers_count, document.json().value("buffers", gltf::Json::array()).size());
        CHECK_EQ(document.files().front().name, f.model.filename().u8string());
        CHECK(document.files().front().size > 0);
        // Image headers agree with expected.json, which lists images by index.
        const auto images = f.expected.find("images");
        if (images == f.expected.end() || !images->is_array()) {
            continue;
        }
        CHECK_EQ(images->size(), document.data().images_count);
        for (std::size_t i = 0; i < images->size() && i < document.data().images_count; ++i) {
            const auto& expected = (*images)[i];
            if (document.image(i).missing) {
                continue;
            }
            const auto& header = document.image(i).header;
            CHECK(header.has_value());
            if (header && expected.contains("mimeType")) {
                CHECK_EQ(std::string(gltf::mimeType(header->format)), expected.at("mimeType").get<std::string>());
            }
            if (header && expected.contains("width")) {
                CHECK_EQ(header->width, expected.at("width").get<std::uint32_t>());
                CHECK_EQ(header->height, expected.at("height").get<std::uint32_t>());
            }
        }
    }
}

TEST_CASE(lists_each_referenced_file_once) {
    const Document document = Document::read(fixture("uri-escaping").model);
    const auto& files = document.files();
    CHECK_EQ(files.size(), 5u);
    CHECK(files[0].role == gltf::FileRole::gltf);
    CHECK_EQ(files[1].name, std::string("geo metry \xE2\x9D\xA4.bin"));
    CHECK(files[1].role == gltf::FileRole::buffer && files[1].size == 140);
    CHECK(files[3].role == gltf::FileRole::image);
    CHECK(document.image(1).file == std::optional<std::size_t>(4));
}

TEST_CASE(glb_chunks_and_data_uris_are_read) {
    const Document glb = Document::read(fixture("textured-glb").model);
    CHECK(glb.container() == gltf::Container::glb);
    CHECK_EQ(glb.files().size(), 1u);
    CHECK(!glb.buffer(0).empty());
    CHECK(glb.image(0).origin == gltf::ImageOrigin::buffer_view);
    CHECK(glb.image(0).header.has_value());

    const Document data_uri = Document::read(fixture("data-uri").model);
    CHECK_EQ(data_uri.files().size(), 1u);
    CHECK_EQ(data_uri.buffer(0).size, data_uri.data().buffers[0].size);
}

TEST_CASE(meshopt_fallback_buffer_has_no_data) {
    const Document document = Document::read(fixture("meshopt").model);
    CHECK(!document.buffer(0).empty());
    CHECK(document.buffer(1).empty());
    CHECK(document.data().buffer_views[0].has_meshopt_compression || document.data().buffer_views_count > 1);
}

TEST_CASE(references_outside_the_folder_need_trust) {
    CHECK_EQ(readFailure(fixture("refs-outside").model), std::string("reference-refused"));
    CHECK_EQ(readFailure(fixture("refs-symlink").model), std::string("reference-refused"));
    CHECK_EQ(readFailure(fixture("refs-absolute").model), std::string("reference-refused"));
    CHECK_EQ(readFailure(fixture("refs-outside").model, true), std::string());
    CHECK_EQ(readFailure(fixture("refs-symlink").model, true), std::string());
    // The absolute path doesn't exist, and is never replaced by a file of the same name.
    CHECK_EQ(readFailure(fixture("refs-absolute").model, true), std::string("reference-refused"));
}

TEST_CASE(missing_images_are_recorded_not_rejected) {
    const Document document = Document::read(fixture("missing-image").model);
    CHECK(!document.image(0).missing);
    CHECK(document.image(1).missing);
    CHECK_EQ(document.image(1).reference, std::string("normal.png"));
    CHECK(document.image(1).bytes.empty());
    // Only the model, its .bin and the image that exists.
    CHECK_EQ(document.files().size(), 3u);
}

TEST_CASE(non_gltf_input_is_rejected) {
    CHECK_EQ(readFailure(fixture("triangle").model.parent_path() / "expected.json"), std::string("invalid-gltf"));
    CHECK_EQ(readFailure(fixture("alt-sources").model.parent_path() / "fallback.png"), std::string("not-gltf"));
    try {
        Document::read(fixture("triangle").model.parent_path() / "missing.gltf");
        CHECK(false);
    } catch (const common::Failure& failure) {
        CHECK(failure.exitCode() == common::ExitCode::input_rejected);
    }
}
