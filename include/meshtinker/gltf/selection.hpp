#pragma once

#include "meshtinker/gltf/document.hpp"
#include "meshtinker/gltf/graph.hpp"

#include <array>
#include <optional>
#include <vector>

namespace meshtinker::gltf {

// What the normalized profile keeps of a document (docs/normalized-profile.md,
// "One scene"): the scene three.js's GLTFLoader shows (the default scene, or
// scene 0 when `scene` is unset) and everything it reaches. Everything else is
// dropped, other scenes and orphans alike. Statistics count what is kept, so
// they describe the file `import` writes.
//
// "Reaches" follows every index forEachReference knows. Animations are reached
// through their target nodes: one that targets a kept node is kept, with all
// its targets. A document without scenes keeps everything: there is nothing to
// choose from, and dropping it all would leave an empty file.
struct Selection {
    // The kept scene, if the document has scenes.
    std::optional<std::size_t> scene;
    // By kind, then by source index: kept or dropped.
    std::array<std::vector<bool>, kind_count> kept;

    bool isKept(Kind kind, std::size_t index) const;
    // How many objects of `kind` are dropped.
    std::size_t dropped(Kind kind) const;
};

Selection selectScene(const Document& document);

} // namespace meshtinker::gltf
