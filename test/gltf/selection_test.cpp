#include "meshtinker/gltf/layout.hpp"
#include "meshtinker/gltf/graph.hpp"
#include "meshtinker/gltf/selection.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <algorithm>

using namespace meshtinker;
using gltf::Kind;

TEST_CASE(keeps_the_default_scene_only) {
    // Scene 1 is the default. OrphanNode belongs to no scene and uses mesh 0,
    // which scene 0's only node uses too; mesh 2 is used by nothing.
    const gltf::Document document = gltf::Document::read(test::fixture("scenes").model);
    const gltf::Selection selection = gltf::selectScene(document);
    CHECK(selection.scene == std::optional<std::size_t>(1));
    CHECK(!selection.isKept(Kind::node, 0));
    CHECK(selection.isKept(Kind::node, 1));
    // Orphans are dropped too.
    CHECK(!selection.isKept(Kind::node, 2));
    CHECK(!selection.isKept(Kind::mesh, 0));
    CHECK(selection.isKept(Kind::mesh, 1));
    CHECK(!selection.isKept(Kind::mesh, 2));
    CHECK_EQ(selection.dropped(Kind::scene), 1u);

    common::Diagnostics diagnostics;
    const gltf::Layout layout = gltf::normalizedLayout(document, selection, diagnostics);
    CHECK_EQ(layout.json["scene"].get<int>(), 0);
    CHECK_EQ(layout.json["scenes"].size(), 1u);
    CHECK_EQ(layout.json["scenes"][0]["nodes"][0].get<int>(), 0);
    CHECK_EQ(layout.json["nodes"].size(), 1u);
    CHECK_EQ(layout.json["meshes"].size(), 1u);
    CHECK_EQ(layout.json["nodes"][0]["name"].get<std::string>(), std::string("InSecondScene"));
    CHECK_EQ(layout.json["nodes"][0]["mesh"].get<int>(), 0);
    CHECK(layout.remap[static_cast<std::size_t>(Kind::node)][0] == gltf::Layout::dropped);
    CHECK(diagnostics.empty());
}

TEST_CASE(a_single_scene_drops_nothing) {
    const gltf::Document document = gltf::Document::read(test::fixture("triangle").model);
    gltf::Selection selection = gltf::selectScene(document);
    CHECK(selection.scene.has_value());
    CHECK_EQ(selection.dropped(Kind::node), 0u);
}

TEST_CASE(image_only_buffer_views_are_dropped) {
    const gltf::Document document = gltf::Document::read(test::fixture("textured-glb").model);
    common::Diagnostics diagnostics;
    const gltf::Layout layout = gltf::normalizedLayout(document, gltf::selectScene(document), diagnostics);
    CHECK_EQ(layout.json["bufferViews"].size() + 1, document.json()["bufferViews"].size());
    CHECK_EQ(layout.view_sources.size(), layout.json["bufferViews"].size());
    CHECK_EQ(layout.bin_order.size(), layout.view_sources.size());
    // Accessors point at the renumbered views.
    for (const auto& accessor : layout.json["accessors"]) {
        CHECK(accessor["bufferView"].get<std::size_t>() < layout.json["bufferViews"].size());
    }
}

TEST_CASE(unknown_extensions_are_warned_about_when_renumbering) {
    const gltf::Document document = gltf::Document::read(test::fixture("unknown-ext").model);
    common::Diagnostics diagnostics;
    gltf::normalizedLayout(document, gltf::selectScene(document), diagnostics);
    // Nothing is renumbered in this single-scene glTF: no warning.
    CHECK(diagnostics.empty());
}

TEST_CASE(material_texture_slots_include_extensions) {
    const gltf::Document document = gltf::Document::read(test::fixture("occupancy").model);
    std::vector<std::string> paths;
    for (const auto& material : document.json()["materials"]) {
        for (const gltf::TextureSlot& slot : gltf::textureSlots(material)) {
            paths.push_back(slot.path);
        }
    }
    CHECK(std::find(paths.begin(), paths.end(), "KHR_materials_clearcoat.clearcoatTexture") != paths.end());
    CHECK(std::find(paths.begin(), paths.end(), "emissiveTexture") != paths.end());
}
