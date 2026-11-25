#include "dedup.hpp"

#include "meshtinker/gltf/edit.hpp"
#include "meshtinker/gltf/geometry.hpp"
#include "meshtinker/gltf/graph.hpp"
#include "meshtinker/gltf/layout.hpp"
#include "meshtinker/gltf/selection.hpp"

#include <cgltf.h>

#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <unordered_map>

namespace meshtinker::dedup {
namespace {

using gltf::Json;
using gltf::Kind;
using gltf::Layout;

std::size_t count(const Json& json, Kind kind) {
    const auto found = json.find(gltf::arrayName(kind));
    return found != json.end() && found->is_array() ? found->size() : 0;
}

// Points every reference to an object of `kind` at `target[index]`, in every
// object that forEachReference knows.
void redirect(Json& json, Kind kind, const std::vector<std::size_t>& target) {
    for (std::size_t k = 0; k < gltf::kind_count; ++k) {
        const auto owner = static_cast<Kind>(k);
        const auto array = json.find(gltf::arrayName(owner));
        if (array == json.end() || !array->is_array()) {
            continue;
        }
        for (Json& object : *array) {
            gltf::forEachReference(owner, object, [&](Kind referenced, Json& value) {
                if (referenced == kind && value.is_number_unsigned() && value.get<std::size_t>() < target.size()) {
                    value = target[value.get<std::size_t>()];
                }
            });
        }
    }
}

// The bytes an object holds: `count` elements of `element` bytes, `stride`
// apart from `offset` in `bytes`.
struct Content {
    common::ByteView bytes;
    std::size_t offset = 0;
    std::size_t stride = 0;
    std::size_t element = 0;
    std::size_t count = 0;

    bool contiguous() const { return stride == element; }
    std::string_view at(std::size_t i) const {
        return {reinterpret_cast<const char*>(bytes.data + offset + i * stride), element};
    }
    // A hash of up to 64 elements spread over the content, the same for
    // equal elements whatever their stride: equal hashes are then compared
    // in full (sameContent), so reading every byte is left to candidates.
    std::size_t hash() const {
        std::size_t h = count;
        const std::size_t step = count > 64 ? count / 64 : 1;
        for (std::size_t i = 0; i < count; i += step) {
            h = (h * 1099511628211ULL) ^ std::hash<std::string_view>{}(at(i));
        }
        return h;
    }
    std::uint64_t size() const { return std::uint64_t{element} * count; }
};

bool sameContent(const Content& a, const Content& b) {
    if (a.element != b.element || a.count != b.count) {
        return false;
    }
    if (a.contiguous() && b.contiguous()) {
        return a.size() == 0 || std::memcmp(a.bytes.data + a.offset, b.bytes.data + b.offset, a.element * a.count) == 0;
    }
    for (std::size_t i = 0; i < a.count; ++i) {
        if (a.at(i) != b.at(i)) {
            return false;
        }
    }
    return true;
}

// By index, the index of the first object equal to it: itself if none.
using Merge = std::vector<std::size_t>;

// Merges objects with equal keys, and, when `content` is given, equal bytes:
// each one into the first of its kind. Objects without a key stay as they are.
// Bytes are read only for objects whose key another one shares.
Merge merge(const std::vector<std::optional<std::string>>& keys,
            const std::function<std::optional<Content>(std::size_t)>& content) {
    Merge result(keys.size());
    std::unordered_map<std::string, std::vector<std::size_t>> groups;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        result[i] = i;
        if (const std::optional<std::string>& key = keys[i]; key.has_value()) {
            groups[key.value()].push_back(i);
        }
    }
    for (const auto& [key, members] : groups) {
        if (members.size() < 2) {
            continue;
        }
        if (!content) {
            for (const std::size_t member : members) {
                result[member] = members.front();
            }
            continue;
        }
        // Representatives by hash: the first object with each content.
        std::unordered_map<std::size_t, std::vector<std::pair<std::size_t, Content>>> firsts;
        for (const std::size_t member : members) {
            const std::optional<Content> bytes = content(member);
            if (!bytes) {
                continue;
            }
            std::vector<std::pair<std::size_t, Content>>& candidates = firsts[bytes->hash()];
            bool found = false;
            for (const auto& [first, first_bytes] : candidates) {
                if (sameContent(first_bytes, *bytes)) {
                    result[member] = first;
                    found = true;
                    break;
                }
            }
            if (!found) {
                candidates.emplace_back(member, *bytes);
            }
        }
    }
    return result;
}

// The object's JSON without its name: glTF-Transform's dedup() compares
// objects without their names too (its keepUniqueNames option, off by default).
std::string withoutName(const Json& object) {
    if (!object.is_object()) {
        return object.dump();
    }
    Json copy = object;
    copy.erase("name");
    return copy.dump();
}

// Merges the objects of `kind` whose JSON is equal but for their name, and
// points every reference at the one kept.
void mergeJson(Json& json, Kind kind) {
    const std::size_t n = count(json, kind);
    if (n < 2) {
        return;
    }
    std::vector<std::optional<std::string>> keys(n);
    for (std::size_t i = 0; i < n; ++i) {
        keys[i] = withoutName(json[gltf::arrayName(kind)][i]);
    }
    redirect(json, kind, merge(keys, nullptr));
}

// Images with the same bytes, whatever their name, URI or MIME type, as
// glTF-Transform compares them. Missing images stay apart.
void mergeImages(const gltf::Document& document, Layout& layout) {
    const std::size_t n = count(layout.json, Kind::image);
    std::vector<std::optional<std::string>> keys(n);
    std::vector<std::optional<Content>> contents(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t source = layout.image_sources[i];
        if (source == Layout::added_image) {
            continue;
        }
        const gltf::ImageData& image = document.image(source);
        if (image.bytes.size == 0) {
            continue;
        }
        keys[i] = std::to_string(image.bytes.size);
        contents[i] = Content{image.bytes, 0, image.bytes.size, image.bytes.size, 1};
    }
    redirect(layout.json, Kind::image, merge(keys, [&](std::size_t i) { return contents[i]; }));
}

// Accessors with the same JSON but for their name and place (`bufferView`,
// `byteOffset`), the same kind of view, and the same elements. Accessors
// without a view, sparse ones, and those in meshopt views, whose bytes are
// compressed, stay apart; so do byte and short matrices, whose columns are
// padded.
void mergeAccessors(const gltf::Document& document, Layout& layout) {
    const cgltf_data& data = document.data();
    const std::size_t n = count(layout.json, Kind::accessor);
    std::vector<std::optional<std::string>> keys(n);
    std::vector<std::optional<Content>> contents(n);
    const std::vector<std::size_t>& remap = layout.remap[static_cast<std::size_t>(Kind::accessor)];
    for (std::size_t source = 0; source < data.accessors_count && source < remap.size(); ++source) {
        const std::size_t i = remap[source];
        if (i == Layout::dropped || i >= n) {
            continue;
        }
        const cgltf_accessor& accessor = data.accessors[source];
        if (accessor.buffer_view == nullptr || accessor.is_sparse || gltf::usesMeshopt(accessor)) {
            continue;
        }
        const std::size_t component = cgltf_component_size(accessor.component_type);
        const bool matrix = accessor.type == cgltf_type_mat2 || accessor.type == cgltf_type_mat3;
        if (component == 0 || (matrix && component < 4)) {
            continue;
        }
        const auto view = static_cast<std::size_t>(accessor.buffer_view - data.buffer_views);
        const common::ByteView bytes = document.bufferView(view);
        Content content{bytes, accessor.offset, accessor.stride, cgltf_num_components(accessor.type) * component,
                        accessor.count};
        if (content.count > 0 &&
            (content.stride < content.element ||
             !bytes.contains(content.offset, content.stride * (content.count - 1) + content.element))) {
            continue;
        }
        Json key = layout.json["accessors"][i];
        key.erase("name");
        key.erase("bufferView");
        key.erase("byteOffset");
        keys[i] = key.dump() + "|" + std::to_string(static_cast<int>(accessor.buffer_view->type));
        contents[i] = content;
    }
    redirect(layout.json, Kind::accessor, merge(keys, [&](std::size_t i) { return contents[i]; }));
}

// The bytes of every buffer view, by `byteLength`.
std::uint64_t viewBytes(const Json& json) {
    std::uint64_t total = 0;
    if (json.contains("bufferViews")) {
        for (const Json& view : json["bufferViews"]) {
            total += view.value("byteLength", std::uint64_t{0});
        }
    }
    return total;
}

// The bytes of the images, each stored image once: two images of one file
// count once.
std::uint64_t imageBytes(const gltf::Document& document, const Layout& layout) {
    std::set<const std::uint8_t*> seen;
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < count(layout.json, Kind::image) && i < layout.image_sources.size(); ++i) {
        const std::size_t source = layout.image_sources[i];
        if (source == Layout::added_image) {
            continue;
        }
        const common::ByteView bytes = document.image(source).bytes;
        if (bytes.size > 0 && seen.insert(bytes.data).second) {
            total += bytes.size;
        }
    }
    return total;
}

} // namespace

Report dedup(const std::filesystem::path& input, const std::filesystem::path& directory, const std::string& stem,
             const Options& options, common::Outputs& outputs, common::Diagnostics& diagnostics) {
    gltf::Document document = gltf::readNormalized(input, options.read, diagnostics);
    Layout layout = gltf::normalizedLayout(document, gltf::selectScene(document), diagnostics);
    Json& json = layout.json;
    Report report;
    const std::pair<Count*, Kind> kinds[] = {
        {&report.images, Kind::image},       {&report.samplers, Kind::sampler},
        {&report.textures, Kind::texture},   {&report.materials, Kind::material},
        {&report.accessors, Kind::accessor}, {&report.meshes, Kind::mesh},
    };
    for (const auto& [field, kind] : kinds) {
        field->before = count(json, kind);
    }
    report.view_bytes.before = viewBytes(json);
    report.image_bytes.before = imageBytes(document, layout);

    // Each kind once the kinds it refers to are merged: textures compare
    // their images and samplers as merged, materials their textures, meshes
    // their accessors and materials.
    mergeImages(document, layout);
    mergeJson(json, Kind::sampler);
    mergeJson(json, Kind::texture);
    mergeJson(json, Kind::material);
    mergeAccessors(document, layout);
    mergeJson(json, Kind::mesh);

    // What nothing refers to any more goes.
    gltf::prune(layout);
    for (const auto& [field, kind] : kinds) {
        field->after = count(json, kind);
    }
    report.view_bytes.after = viewBytes(json);
    report.image_bytes.after = imageBytes(document, layout);
    if (!options.dry_run) {
        report.files = gltf::writeSeparate(document, layout, directory, stem, outputs);
    }
    return report;
}

} // namespace meshtinker::dedup
