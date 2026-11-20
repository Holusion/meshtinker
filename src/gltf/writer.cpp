#include "meshtinker/gltf/writer.hpp"

#include "meshtinker/common/failure.hpp"
#include "meshtinker/gltf/uri.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <set>

namespace meshtinker::gltf {
namespace fs = std::filesystem;
using common::ByteView;

namespace {

constexpr std::uint64_t view_alignment = 4;
constexpr const char* meshopt_extensions[] = {"EXT_meshopt_compression", "KHR_meshopt_compression"};

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return text;
}

const char* extensionFor(const ImageData& image, const Json& json) {
    if (image.header) {
        switch (image.header->format) {
        case ImageFormat::png:
            return "png";
        case ImageFormat::jpeg:
            return "jpg";
        case ImageFormat::webp:
            return "webp";
        case ImageFormat::ktx2:
            return "ktx2";
        }
    }
    const std::string declared = json.value("mimeType", std::string());
    if (declared == "image/png") {
        return "png";
    }
    if (declared == "image/jpeg") {
        return "jpg";
    }
    if (declared == "image/webp") {
        return "webp";
    }
    if (declared == "image/ktx2") {
        return "ktx2";
    }
    return "bin";
}

// The meshopt extension object of a buffer view, if any.
const Json* meshoptExtension(const Json& view) {
    const auto extensions = view.find("extensions");
    if (extensions == view.end() || !extensions->is_object()) {
        return nullptr;
    }
    for (const char* name : meshopt_extensions) {
        const auto extension = extensions->find(name);
        if (extension != extensions->end() && extension->is_object()) {
            return &*extension;
        }
    }
    return nullptr;
}

Json* meshoptExtension(Json& view) { return const_cast<Json*>(meshoptExtension(static_cast<const Json&>(view))); }

// The name of the meshopt extension of a buffer view, if any.
const char* meshoptExtensionName(const Json& view) {
    const auto extensions = view.find("extensions");
    if (extensions == view.end() || !extensions->is_object()) {
        return nullptr;
    }
    for (const char* name : meshopt_extensions) {
        if (extensions->contains(name)) {
            return name;
        }
    }
    return nullptr;
}

std::uint64_t alignUp(std::uint64_t n, std::uint64_t alignment) { return (n + alignment - 1) / alignment * alignment; }

} // namespace

std::vector<std::string> imageFileNames(const Document& document, const std::string& stem) {
    const cgltf_data& data = document.data();
    const Json& json = document.json();
    std::vector<std::string> names(data.images_count);
    std::set<std::string> taken{lowercase(stem + ".gltf"), lowercase(stem + ".bin")};
    std::map<std::size_t, std::string> by_file;
    for (std::size_t i = 0; i < data.images_count; ++i) {
        const ImageData& image = document.image(i);
        if (image.file) {
            if (const auto found = by_file.find(*image.file); found != by_file.end()) {
                names[i] = found->second;
                continue;
            }
        }
        const std::string extension = extensionFor(image, json["images"][i]);
        std::string name;
        if (image.file || image.missing) {
            name = fs::u8path(image.reference).filename().u8string();
        }
        std::string generated = stem;
        generated.append("_texture_").append(std::to_string(i));
        if (name.empty() || taken.count(lowercase(name)) != 0) {
            name = generated;
            name.append(".").append(extension);
        }
        for (int n = 1; taken.count(lowercase(name)) != 0; ++n) {
            name = generated;
            name.append("_").append(std::to_string(n)).append(".").append(extension);
        }
        taken.insert(lowercase(name));
        names[i] = name;
        if (image.file) {
            by_file.emplace(*image.file, name);
        }
    }
    return names;
}

std::vector<WrittenFile> writeSeparate(const Document& document, const Layout& layout, const fs::path& directory,
                                       const std::string& stem, common::Outputs& outputs) {
    const cgltf_data& data = document.data();
    Json json = layout.json;
    std::vector<WrittenFile> written;
    written.push_back({FileRole::gltf, stem + ".gltf", 0});

    // Buffers without data that a kept view still names keep their own entry,
    // after the .bin (buffer 0) if there is one.
    bool any_data = false;
    std::vector<bool> needed(data.buffers_count, false);
    for (std::size_t i = 0; i < layout.view_sources.size(); ++i) {
        const std::size_t view = layout.view_sources[i];
        if (view == Layout::added_view) {
            any_data = any_data || !layout.added.at(i).empty();
            continue;
        }
        const cgltf_buffer_view& source = data.buffer_views[view];
        const auto buffer = static_cast<std::size_t>(source.buffer - data.buffers);
        needed[buffer] = true;
        any_data = any_data || !document.buffer(buffer).empty();
        if (source.has_meshopt_compression) {
            const auto compressed = static_cast<std::size_t>(source.meshopt_compression.buffer - data.buffers);
            needed[compressed] = true;
            any_data = any_data || !document.buffer(compressed).empty();
        }
    }
    std::vector<std::size_t> new_buffer(data.buffers_count, 0);
    Json buffers = Json::array();
    if (any_data) {
        buffers.push_back(Json::object());
    }
    for (std::size_t i = 0; i < data.buffers_count; ++i) {
        if (needed[i] && document.buffer(i).empty()) {
            new_buffer[i] = buffers.size();
            buffers.push_back(document.json()["buffers"][i]);
        }
    }
    // Added meshopt views get one new fallback buffer per extension, after
    // the others, as glTF-Transform and gltfpack write them: it holds no data,
    // and its length covers the views' decoded ranges.
    std::map<std::string, std::size_t> fallback_buffer;
    std::map<std::string, std::uint64_t> fallback_length;
    for (const auto& [index, bytes] : layout.added) {
        const char* name = meshoptExtensionName(json["bufferViews"][index]);
        if (name != nullptr && fallback_buffer.count(name) == 0) {
            fallback_buffer[name] = buffers.size();
            fallback_length[name] = 0;
            buffers.push_back(Json{{"byteLength", 0}, {"extensions", {{name, {{"fallback", true}}}}}});
        }
    }

    if (any_data) {
        const std::string bin_name = stem + ".bin";
        common::OutputFile bin = outputs.create(directory / fs::u8path(bin_name));
        for (const std::size_t index : layout.bin_order) {
            const std::size_t source_index = layout.view_sources[index];
            Json& view = json["bufferViews"][index];
            if (source_index == Layout::added_view) {
                const ByteView bytes = layout.added.at(index);
                const char* name = meshoptExtensionName(view);
                if (name != nullptr) {
                    std::uint64_t& length = fallback_length[name];
                    length = alignUp(length, view_alignment);
                    view["buffer"] = fallback_buffer[name];
                    view["byteOffset"] = length;
                    length += view.value("byteLength", std::uint64_t{0});
                }
                // Taken after the view's own keys are set: adding keys to an
                // ordered_json object moves its values.
                Json& target = name != nullptr ? *meshoptExtension(view) : view;
                bin.align(view_alignment);
                target["buffer"] = 0;
                target["byteOffset"] = bin.position();
                bin.write(bytes);
                continue;
            }
            const cgltf_buffer_view& source = data.buffer_views[source_index];
            const auto source_buffer = static_cast<std::size_t>(source.buffer - data.buffers);
            view["buffer"] = new_buffer[source_buffer];
            const ByteView bytes = document.bufferView(source_index);
            if (!bytes.empty() || source.size == 0) {
                bin.align(view_alignment);
                view["byteOffset"] = bin.position();
                bin.write(bytes);
            }
            Json* extension = meshoptExtension(view);
            if (extension != nullptr && source.has_meshopt_compression) {
                const cgltf_meshopt_compression& compression = source.meshopt_compression;
                const auto compressed_buffer = static_cast<std::size_t>(compression.buffer - data.buffers);
                (*extension)["buffer"] = new_buffer[compressed_buffer];
                const ByteView compressed = document.buffer(compressed_buffer).sub(compression.offset, compression.size);
                if (!compressed.empty()) {
                    bin.align(view_alignment);
                    (*extension)["byteOffset"] = bin.position();
                    bin.write(compressed);
                }
            }
        }
        bin.align(view_alignment);
        buffers[0] = Json{{"byteLength", bin.position()}, {"uri", encodeUriSegment(bin_name)}};
        for (const auto& [name, buffer] : fallback_buffer) {
            buffers[buffer]["byteLength"] = fallback_length[name];
        }
        written.push_back({FileRole::buffer, bin_name, bin.position()});
        bin.close();
    }
    if (buffers.empty()) {
        json.erase("buffers");
    } else {
        json["buffers"] = std::move(buffers);
    }

    const std::vector<std::string> names = imageFileNames(document, stem);
    std::set<std::string> done;
    for (std::size_t i = 0; i < layout.image_sources.size(); ++i) {
        const std::size_t source = layout.image_sources[i];
        Json& image = json["images"][i];
        image.erase("bufferView");
        image["uri"] = encodeUriSegment(names[source]);
        // A missing image keeps a reference to its name, with no file behind it.
        if (!done.insert(names[source]).second || document.image(source).missing) {
            continue;
        }
        common::OutputFile file = outputs.create(directory / fs::u8path(names[source]));
        file.write(document.image(source).bytes);
        written.push_back({FileRole::image, names[source], file.position()});
        file.close();
    }

    common::OutputFile gltf = outputs.create(directory / fs::u8path(stem + ".gltf"));
    gltf.write(json.dump());
    written.front().size = gltf.position();
    gltf.close();
    return written;
}

} // namespace meshtinker::gltf
