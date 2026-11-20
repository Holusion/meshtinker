#include "meshtinker/gltf/layout.hpp"

#include <algorithm>
#include <numeric>
#include <set>
#include <utility>

namespace meshtinker::gltf {
namespace {

std::size_t kindIndex(Kind kind) { return static_cast<std::size_t>(kind); }

std::size_t count(const Json& json, Kind kind) {
    const auto found = json.find(arrayName(kind));
    return found != json.end() && found->is_array() ? found->size() : 0;
}

std::vector<std::size_t> iota(std::size_t n) {
    std::vector<std::size_t> values(n);
    std::iota(values.begin(), values.end(), std::size_t{0});
    return values;
}

// Buffer views in the order primitives use them, then the rest in index order.
std::vector<std::size_t> primitiveOrder(const Json& json, const std::vector<bool>& kept_views) {
    std::vector<std::size_t> order;
    std::vector<bool> placed(kept_views.size(), false);
    const auto place = [&](const Json& value) {
        if (value.is_number_unsigned()) {
            const auto view = value.get<std::size_t>();
            if (view < placed.size() && kept_views[view] && !placed[view]) {
                placed[view] = true;
                order.push_back(view);
            }
        }
    };
    const auto placeAccessor = [&](const Json& value) {
        if (!value.is_number_unsigned() || value.get<std::size_t>() >= count(json, Kind::accessor)) {
            return;
        }
        forEachReference(Kind::accessor, json["accessors"][value.get<std::size_t>()],
                         [&](Kind, const Json& view) { place(view); });
    };
    if (json.contains("meshes")) {
        for (const Json& mesh : json["meshes"]) {
            if (!mesh.contains("primitives")) {
                continue;
            }
            for (const Json& primitive : mesh["primitives"]) {
                if (primitive.contains("indices")) {
                    placeAccessor(primitive["indices"]);
                }
                if (primitive.contains("attributes") && primitive["attributes"].is_object()) {
                    for (const auto& attribute : primitive["attributes"].items()) {
                        placeAccessor(attribute.value());
                    }
                }
                if (primitive.contains("targets") && primitive["targets"].is_array()) {
                    for (const Json& target : primitive["targets"]) {
                        for (const auto& attribute : target.items()) {
                            placeAccessor(attribute.value());
                        }
                    }
                }
                const auto extensions = primitive.find("extensions");
                if (extensions != primitive.end() && extensions->contains("KHR_draco_mesh_compression")) {
                    const Json& draco = (*extensions)["KHR_draco_mesh_compression"];
                    if (draco.contains("bufferView")) {
                        place(draco["bufferView"]);
                    }
                }
            }
        }
    }
    for (std::size_t view = 0; view < kept_views.size(); ++view) {
        if (kept_views[view] && !placed[view]) {
            order.push_back(view);
        }
    }
    return order;
}

} // namespace

std::vector<std::size_t> binOrder(const Json& json) {
    return primitiveOrder(json, std::vector<bool>(count(json, Kind::buffer_view), true));
}

Layout identityLayout(const Document& document) {
    Layout layout;
    layout.json = document.json();
    layout.view_sources = iota(count(layout.json, Kind::buffer_view));
    layout.image_sources = iota(count(layout.json, Kind::image));
    layout.bin_order = layout.view_sources;
    for (std::size_t k = 0; k < kind_count; ++k) {
        layout.remap[k] = iota(count(layout.json, static_cast<Kind>(k)));
    }
    return layout;
}

Layout normalizedLayout(const Document& document, const Selection& selection, common::Diagnostics& diagnostics) {
    const Json& source = document.json();
    std::array<std::vector<bool>, kind_count> keep = selection.kept;

    // Buffer views only images use are dropped: images are written as files.
    std::vector<bool>& views = keep[kindIndex(Kind::buffer_view)];
    std::vector<bool> used_by_data(views.size(), false);
    std::vector<bool> used_by_image(views.size(), false);
    for (std::size_t k = 0; k < kind_count; ++k) {
        const auto kind = static_cast<Kind>(k);
        for (std::size_t i = 0; i < keep[k].size(); ++i) {
            if (!keep[k][i]) {
                continue;
            }
            forEachReference(kind, source[arrayName(kind)][i], [&](Kind target, const Json& value) {
                if (target != Kind::buffer_view || !value.is_number_unsigned() || value.get<std::size_t>() >= views.size()) {
                    return;
                }
                (kind == Kind::image ? used_by_image : used_by_data)[value.get<std::size_t>()] = true;
            });
        }
    }
    for (std::size_t v = 0; v < views.size(); ++v) {
        if (used_by_image[v] && !used_by_data[v]) {
            views[v] = false;
        }
    }

    // New index of each kept object, by kind.
    Layout layout;
    std::array<std::vector<std::size_t>, kind_count>& remap = layout.remap;
    bool renumbered = false;
    for (std::size_t k = 0; k < kind_count; ++k) {
        remap[k].assign(keep[k].size(), Layout::dropped);
        std::size_t next = 0;
        for (std::size_t i = 0; i < keep[k].size(); ++i) {
            if (keep[k][i]) {
                renumbered = renumbered || next != i;
                remap[k][i] = next++;
            } else {
                renumbered = true;
            }
        }
    }

    layout.json = Json::object();
    // Copy the root in source order, filtering the arrays.
    for (const auto& item : source.items()) {
        const std::string& key = item.key();
        bool is_kind_array = false;
        for (std::size_t k = 0; k < kind_count; ++k) {
            const auto kind = static_cast<Kind>(k);
            if (key != arrayName(kind)) {
                continue;
            }
            is_kind_array = true;
            Json filtered = Json::array();
            for (std::size_t i = 0; i < keep[k].size(); ++i) {
                if (!keep[k][i]) {
                    continue;
                }
                Json element = item.value()[i];
                // Buffer references stay source indices for the writer.
                if (kind != Kind::buffer_view) {
                    forEachReference(kind, element, [&](Kind target, Json& value) {
                        if (value.is_number_unsigned() && value.get<std::size_t>() < remap[kindIndex(target)].size()) {
                            value = remap[kindIndex(target)][value.get<std::size_t>()];
                        }
                    });
                }
                filtered.push_back(std::move(element));
            }
            // Empty arrays are invalid glTF: leave the property out.
            if (!filtered.empty()) {
                layout.json[key] = std::move(filtered);
            }
            if (kind == Kind::buffer_view) {
                for (std::size_t i = 0; i < keep[k].size(); ++i) {
                    if (keep[k][i]) {
                        layout.view_sources.push_back(i);
                    }
                }
            }
            if (kind == Kind::image) {
                for (std::size_t i = 0; i < keep[k].size(); ++i) {
                    if (keep[k][i]) {
                        layout.image_sources.push_back(i);
                    }
                }
            }
        }
        if (!is_kind_array) {
            layout.json[key] = item.value();
        }
    }
    if (selection.scene) {
        layout.json["scene"] = 0;
    }

    // Bin order, in output view indices.
    std::vector<bool> kept_views(views.size());
    for (std::size_t v = 0; v < views.size(); ++v) {
        kept_views[v] = views[v];
    }
    for (const std::size_t view : primitiveOrder(source, kept_views)) {
        layout.bin_order.push_back(remap[kindIndex(Kind::buffer_view)][view]);
    }

    if (renumbered) {
        const auto used = source.find("extensionsUsed");
        if (used != source.end() && used->is_array()) {
            for (const Json& name : *used) {
                if (name.is_string() && !isIndexAwareExtension(name.get<std::string>())) {
                    diagnostics.warn("extension-indices-not-renumbered",
                                     "Objects were dropped or renumbered, but this extension is unknown: indices it "
                                     "holds were left as they were",
                                     name.get<std::string>());
                }
            }
        }
    }
    return layout;
}

Document readNormalized(const std::filesystem::path& path, const ReadOptions& options,
                        common::Diagnostics& diagnostics) {
    Document source = Document::read(path, options);
    const Selection selection = selectScene(source);
    const Layout layout = normalizedLayout(source, selection, diagnostics);
    return Document::normalized(std::move(source), layout);
}

} // namespace meshtinker::gltf
