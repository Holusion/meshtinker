#include "meshtinker/gltf/edit.hpp"

#include "meshtinker/gltf/selection.hpp"

#include <algorithm>
#include <vector>

namespace meshtinker::gltf {
namespace {

constexpr int unsigned_byte_type = 5121;
constexpr int unsigned_short_type = 5123;
constexpr int float_type = 5126;

std::size_t kindIndex(Kind kind) { return static_cast<std::size_t>(kind); }

std::size_t count(const Json& json, Kind kind) {
    const auto found = json.find(arrayName(kind));
    return found != json.end() && found->is_array() ? found->size() : 0;
}

Json& array(Json& json, Kind kind) {
    Json& value = json[arrayName(kind)];
    if (!value.is_array()) {
        value = Json::array();
    }
    return value;
}

void removeName(Json& json, const char* list, const std::string& name) {
    const auto found = json.find(list);
    if (found == json.end() || !found->is_array()) {
        return;
    }
    Json kept = Json::array();
    for (const Json& value : *found) {
        if (!(value.is_string() && value.get<std::string>() == name)) {
            kept.push_back(value);
        }
    }
    if (kept.empty()) {
        json.erase(list);
    } else {
        *found = std::move(kept);
    }
}

void dropExtension(Json& json, const std::string& name) {
    removeName(json, "extensionsUsed", name);
    removeName(json, "extensionsRequired", name);
}

bool hasExtension(const Json& object, const char* name) {
    const auto extensions = object.find("extensions");
    return extensions != object.end() && extensions->is_object() && extensions->contains(name);
}

template <typename Visit>
void forEachPrimitive(const Json& json, Visit visit) {
    const auto meshes = json.find("meshes");
    if (meshes == json.end() || !meshes->is_array()) {
        return;
    }
    for (const Json& mesh : *meshes) {
        const auto primitives = mesh.find("primitives");
        if (primitives == mesh.end() || !primitives->is_array()) {
            continue;
        }
        for (const Json& primitive : *primitives) {
            visit(primitive);
        }
    }
}

// Whether core glTF 2.0 allows `accessor` for attribute `name` (spec 3.7.2.1).
bool coreAllows(const std::string& name, const Json& accessor, bool morph_target) {
    const int type = accessor.value("componentType", 0);
    const bool normalized = accessor.value("normalized", false);
    if (name.rfind('_', 0) == 0) {
        return true;
    }
    if (type == float_type) {
        return true;
    }
    if (morph_target || name == "POSITION" || name == "NORMAL" || name == "TANGENT") {
        return false;
    }
    if (name.rfind("JOINTS_", 0) == 0) {
        return type == unsigned_byte_type || type == unsigned_short_type;
    }
    if (name.rfind("TEXCOORD_", 0) == 0 || name.rfind("COLOR_", 0) == 0 || name.rfind("WEIGHTS_", 0) == 0) {
        return normalized && (type == unsigned_byte_type || type == unsigned_short_type);
    }
    return true;
}

} // namespace

std::size_t addView(Layout& layout, Json view, common::ByteView bytes) {
    Json& views = array(layout.json, Kind::buffer_view);
    const std::size_t index = views.size();
    views.push_back(std::move(view));
    layout.view_sources.push_back(Layout::added_view);
    layout.added[index] = bytes;
    layout.bin_order.push_back(index);
    return index;
}

std::size_t addAccessor(Layout& layout, Json accessor) {
    Json& accessors = array(layout.json, Kind::accessor);
    accessors.push_back(std::move(accessor));
    return accessors.size() - 1;
}

void replaceGeometry(Layout& layout, std::size_t mesh, std::size_t primitive, const PrimitiveGeometry& geometry) {
    Json& target = layout.json["meshes"][mesh]["primitives"][primitive];
    target["attributes"] = geometry.attributes;
    if (geometry.indices) {
        target["indices"] = *geometry.indices;
    } else {
        target.erase("indices");
    }
    if (geometry.targets.is_array() && !geometry.targets.empty()) {
        target["targets"] = geometry.targets;
    } else {
        target.erase("targets");
    }
    if (geometry.mode == 4) {
        target.erase("mode");
    } else {
        target["mode"] = geometry.mode;
    }
    const auto extensions = target.find("extensions");
    if (extensions != target.end() && extensions->is_object()) {
        extensions->erase("KHR_draco_mesh_compression");
        if (extensions->empty()) {
            target.erase("extensions");
        }
    }
}

void copyAccessorProperties(const Layout& layout, std::size_t source, Json& accessor) {
    if (source >= count(layout.json, Kind::accessor)) {
        return;
    }
    const Json& from = layout.json["accessors"][source];
    for (const char* key : {"name", "extras", "extensions"}) {
        if (from.contains(key)) {
            accessor[key] = from[key];
        }
    }
}

void useExtension(Json& json, const std::string& name, bool required) {
    std::vector<const char*> lists{"extensionsUsed"};
    if (required) {
        lists.push_back("extensionsRequired");
    }
    for (const char* list : lists) {
        Json& names = json[list];
        if (!names.is_array()) {
            names = Json::array();
        }
        if (std::find(names.begin(), names.end(), Json(name)) == names.end()) {
            names.push_back(name);
        }
    }
}

bool needsMeshQuantization(const Json& json) {
    const std::size_t accessors = count(json, Kind::accessor);
    bool needed = false;
    const auto check = [&](const Json& attributes, bool morph_target) {
        if (!attributes.is_object()) {
            return;
        }
        for (const auto& attribute : attributes.items()) {
            if (attribute.value().is_number_unsigned() && attribute.value().get<std::size_t>() < accessors &&
                !coreAllows(attribute.key(), json["accessors"][attribute.value().get<std::size_t>()], morph_target)) {
                needed = true;
            }
        }
    };
    forEachPrimitive(json, [&](const Json& primitive) {
        if (primitive.contains("attributes")) {
            check(primitive["attributes"], false);
        }
        if (primitive.contains("targets") && primitive["targets"].is_array()) {
            for (const Json& target : primitive["targets"]) {
                check(target, true);
            }
        }
    });
    return needed;
}

void removePrimitive(Layout& layout, std::size_t mesh, std::size_t primitive) {
    Json& primitives = layout.json["meshes"][mesh]["primitives"];
    primitives.erase(primitive);
}

void prune(Layout& layout) {
    Json& json = layout.json;

    // A mesh without primitives is invalid glTF: it goes, and the nodes that
    // drew it keep their place without it. Their morph weights, instancing
    // and weight animations go with it.
    std::vector<bool> empty_mesh(count(json, Kind::mesh), false);
    for (std::size_t m = 0; m < empty_mesh.size(); ++m) {
        const Json& mesh = json["meshes"][m];
        empty_mesh[m] = !mesh.contains("primitives") || mesh["primitives"].empty();
    }
    std::vector<bool> lost_mesh(count(json, Kind::node), false);
    for (std::size_t n = 0; n < lost_mesh.size(); ++n) {
        Json& node = json["nodes"][n];
        const auto mesh = node.find("mesh");
        if (mesh == node.end() || !mesh->is_number_unsigned() || mesh->get<std::size_t>() >= empty_mesh.size() ||
            !empty_mesh[mesh->get<std::size_t>()]) {
            continue;
        }
        lost_mesh[n] = true;
        node.erase("mesh");
        node.erase("weights");
        if (node.contains("extensions")) {
            node["extensions"].erase("EXT_mesh_gpu_instancing");
            if (node["extensions"].empty()) {
                node.erase("extensions");
            }
        }
    }
    for (std::size_t a = 0; a < count(json, Kind::animation); ++a) {
        Json& animation = json["animations"][a];
        if (!animation.contains("channels")) {
            continue;
        }
        Json channels = Json::array();
        for (Json& channel : animation["channels"]) {
            const Json& target = channel.value("target", Json::object());
            const bool weights = target.value("path", "") == "weights";
            const auto node = target.find("node");
            const bool lost = weights && node != target.end() && node->is_number_unsigned() &&
                              node->get<std::size_t>() < lost_mesh.size() && lost_mesh[node->get<std::size_t>()];
            if (!lost) {
                channels.push_back(std::move(channel));
            }
        }
        // An animation left without channels targets nothing, and isn't reached below.
        animation["channels"] = std::move(channels);
    }

    // What the scene still reaches stays, as import selects it
    // (gltf::selectScene); buffers stay, since the writer places their data.
    Selection selection = selectScene(json);
    for (std::size_t m = 0; m < empty_mesh.size(); ++m) {
        if (empty_mesh[m]) {
            selection.kept[kindIndex(Kind::mesh)][m] = false;
        }
    }
    std::fill(selection.kept[kindIndex(Kind::buffer)].begin(), selection.kept[kindIndex(Kind::buffer)].end(), true);

    std::array<std::vector<std::size_t>, kind_count> maps;
    for (std::size_t k = 0; k < kind_count; ++k) {
        const std::vector<bool>& used = selection.kept[k];
        maps[k].assign(used.size(), Layout::dropped);
        std::size_t next = 0;
        for (std::size_t i = 0; i < used.size(); ++i) {
            if (used[i]) {
                maps[k][i] = next++;
            }
        }
    }
    for (std::size_t k = 0; k < kind_count; ++k) {
        const auto kind = static_cast<Kind>(k);
        for (std::size_t i = 0; i < count(json, kind); ++i) {
            if (!selection.kept[k][i]) {
                continue;
            }
            forEachReference(kind, json[arrayName(kind)][i], [&](Kind target, Json& value) {
                // Buffer views keep naming their source buffers, for the writer.
                if (target == Kind::buffer || !value.is_number_unsigned()) {
                    return;
                }
                const auto index = value.get<std::size_t>();
                const std::vector<std::size_t>& map = maps[kindIndex(target)];
                if (index < map.size()) {
                    value = map[index];
                }
            });
        }
    }
    for (std::size_t k = 0; k < kind_count; ++k) {
        const auto kind = static_cast<Kind>(k);
        if (kind == Kind::buffer || count(json, kind) == 0) {
            continue;
        }
        Json kept = Json::array();
        for (std::size_t i = 0; i < selection.kept[k].size(); ++i) {
            if (selection.kept[k][i]) {
                kept.push_back(std::move(json[arrayName(kind)][i]));
            }
        }
        if (kept.empty()) {
            json.erase(arrayName(kind));
        } else {
            json[arrayName(kind)] = std::move(kept);
        }
    }
    if (json.contains("scenes") && json.contains("scene")) {
        json["scene"] = 0;
    }

    const std::vector<std::size_t>& view_map = maps[kindIndex(Kind::buffer_view)];
    std::vector<std::size_t> view_sources;
    std::map<std::size_t, common::ByteView> added;
    for (std::size_t i = 0; i < view_map.size(); ++i) {
        if (view_map[i] != Layout::dropped) {
            view_sources.push_back(layout.view_sources[i]);
            if (const auto found = layout.added.find(i); found != layout.added.end()) {
                added[view_map[i]] = found->second;
            }
        }
    }
    layout.view_sources = std::move(view_sources);
    layout.added = std::move(added);
    const std::vector<std::size_t>& image_map = maps[kindIndex(Kind::image)];
    std::vector<std::size_t> image_sources;
    for (std::size_t i = 0; i < image_map.size(); ++i) {
        if (image_map[i] != Layout::dropped) {
            image_sources.push_back(layout.image_sources[i]);
        }
    }
    layout.image_sources = std::move(image_sources);
    for (std::size_t k = 0; k < kind_count; ++k) {
        if (static_cast<Kind>(k) == Kind::buffer) {
            continue;
        }
        for (std::size_t& index : layout.remap[k]) {
            if (index != Layout::dropped && index < maps[k].size()) {
                index = maps[k][index];
            }
        }
    }
    layout.bin_order = binOrder(json);

    bool draco = false;
    forEachPrimitive(json, [&](const Json& primitive) {
        draco = draco || hasExtension(primitive, "KHR_draco_mesh_compression");
    });
    if (!draco) {
        dropExtension(json, "KHR_draco_mesh_compression");
    }
    for (const char* name : {"EXT_meshopt_compression", "KHR_meshopt_compression"}) {
        bool used = false;
        for (const Kind kind : {Kind::buffer_view, Kind::buffer}) {
            for (std::size_t i = 0; i < count(json, kind); ++i) {
                used = used || hasExtension(json[arrayName(kind)][i], name);
            }
        }
        if (!used) {
            dropExtension(json, name);
        }
    }
    if (!needsMeshQuantization(json)) {
        dropExtension(json, "KHR_mesh_quantization");
    }
}

} // namespace meshtinker::gltf
