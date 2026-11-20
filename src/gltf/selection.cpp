#include "meshtinker/gltf/selection.hpp"

#include <algorithm>
#include <deque>
#include <utility>

namespace meshtinker::gltf {
namespace {

std::size_t kindIndex(Kind kind) { return static_cast<std::size_t>(kind); }

const Json& element(const Json& json, Kind kind, std::size_t index) {
    return json.at(arrayName(kind)).at(index);
}

std::size_t count(const Json& json, Kind kind) {
    const auto found = json.find(arrayName(kind));
    return found != json.end() && found->is_array() ? found->size() : 0;
}

using Marks = std::array<std::vector<bool>, kind_count>;

Marks emptyMarks(const Json& json) {
    Marks marks;
    for (std::size_t k = 0; k < kind_count; ++k) {
        marks[k].assign(count(json, static_cast<Kind>(k)), false);
    }
    return marks;
}

// Marks everything reachable from `scene`, animations included through their
// targets.
Marks reach(const Json& json, std::size_t scene) {
    Marks marks = emptyMarks(json);
    std::deque<std::pair<Kind, std::size_t>> queue;
    const auto mark = [&](Kind kind, std::size_t index) {
        std::vector<bool>& list = marks[kindIndex(kind)];
        if (index < list.size() && !list[index]) {
            list[index] = true;
            queue.emplace_back(kind, index);
        }
    };
    mark(Kind::scene, scene);
    const std::size_t animation_count = count(json, Kind::animation);
    for (;;) {
        while (!queue.empty()) {
            const auto [kind, index] = queue.front();
            queue.pop_front();
            forEachReference(kind, element(json, kind, index), [&](Kind target, const Json& value) {
                if (value.is_number_unsigned()) {
                    mark(target, value.get<std::size_t>());
                }
            });
        }
        // An animation joins once it targets a reached node, then reaches its other targets.
        bool added = false;
        for (std::size_t a = 0; a < animation_count; ++a) {
            if (marks[kindIndex(Kind::animation)][a]) {
                continue;
            }
            bool targets_reached = false;
            forEachReference(Kind::animation, element(json, Kind::animation, a), [&](Kind target, const Json& value) {
                if (target == Kind::node && value.is_number_unsigned() &&
                    value.get<std::size_t>() < marks[kindIndex(Kind::node)].size() &&
                    marks[kindIndex(Kind::node)][value.get<std::size_t>()]) {
                    targets_reached = true;
                }
            });
            if (targets_reached) {
                mark(Kind::animation, a);
                added = true;
            }
        }
        if (!added) {
            break;
        }
    }
    return marks;
}

} // namespace

bool Selection::isKept(Kind kind, std::size_t index) const {
    const std::vector<bool>& list = kept[kindIndex(kind)];
    return index < list.size() && list[index];
}

std::size_t Selection::dropped(Kind kind) const {
    const std::vector<bool>& list = kept[kindIndex(kind)];
    return static_cast<std::size_t>(std::count(list.begin(), list.end(), false));
}

Selection selectScene(const Document& document) { return selectScene(document.json()); }

Selection selectScene(const Json& json) {
    Selection selection;
    const std::size_t scenes = count(json, Kind::scene);
    if (scenes == 0) {
        selection.kept = emptyMarks(json);
        for (std::vector<bool>& list : selection.kept) {
            std::fill(list.begin(), list.end(), true);
        }
        return selection;
    }
    // three.js GLTFLoader shows `scene`, or scene 0 when it is unset.
    std::size_t scene = 0;
    if (const auto found = json.find("scene"); found != json.end() && found->is_number_unsigned()) {
        scene = std::min(found->get<std::size_t>(), scenes - 1);
    }
    selection.scene = scene;
    // Everything a reached object refers to is reached too, so this is closed
    // under references.
    selection.kept = reach(json, scene);
    return selection;
}

} // namespace meshtinker::gltf
