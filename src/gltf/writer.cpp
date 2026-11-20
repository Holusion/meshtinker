#include "meshtinker/gltf/writer.hpp"

#include "meshtinker/common/failure.hpp"
#include "meshtinker/gltf/uri.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

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

ImageNames::ImageNames(const Document& document, const std::string& stem) {
    taken_.insert(lowercase(stem + ".gltf"));
    taken_.insert(lowercase(stem + ".bin"));
    for (const std::string& name : imageFileNames(document, stem)) {
        taken_.insert(lowercase(name));
    }
}

std::string ImageNames::reserve(const std::string& wanted) {
    const fs::path path = fs::u8path(wanted);
    const std::string base = path.stem().u8string();
    const std::string extension = path.extension().u8string();
    std::string name = wanted;
    for (int n = 1; taken_.count(lowercase(name)) != 0; ++n) {
        name = base;
        name.append("_").append(std::to_string(n)).append(extension);
    }
    taken_.insert(lowercase(name));
    return name;
}

namespace {

// A run of bytes at its offset in the binary data.
struct Piece {
    std::uint64_t offset = 0;
    ByteView bytes;
};

// The binary data a layout writes, the .bin of writeSeparate or a GLB's BIN
// chunk before its images, and the JSON that points into it.
struct BinPlan {
    // The layout's JSON with `buffers` and the views' `buffer` and
    // `byteOffset` set. Buffer 0, when there is data, has only its length.
    Json json;
    bool any_data = false;
    std::vector<Piece> pieces;
    // Each piece starts 4-byte aligned, and the length is too.
    std::uint64_t length = 0;
};

// `data_buffer` reserves buffer 0 for data even when no view has any, for a
// GLB whose BIN chunk holds only images.
BinPlan planBin(const Document& document, const Layout& layout, bool data_buffer = false) {
    const cgltf_data& data = document.data();
    BinPlan plan;
    plan.json = layout.json;
    Json& json = plan.json;

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
    any_data = any_data || data_buffer;
    plan.any_data = any_data;
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
        std::uint64_t& position = plan.length;
        const auto place = [&](ByteView bytes) {
            position = alignUp(position, view_alignment);
            plan.pieces.push_back({position, bytes});
            const std::uint64_t offset = position;
            position += bytes.size;
            return offset;
        };
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
                target["buffer"] = 0;
                target["byteOffset"] = place(bytes);
                continue;
            }
            const cgltf_buffer_view& source = data.buffer_views[source_index];
            const auto source_buffer = static_cast<std::size_t>(source.buffer - data.buffers);
            view["buffer"] = new_buffer[source_buffer];
            const ByteView bytes = document.bufferView(source_index);
            if (!bytes.empty() || source.size == 0) {
                view["byteOffset"] = place(bytes);
            }
            Json* extension = meshoptExtension(view);
            if (extension != nullptr && source.has_meshopt_compression) {
                const cgltf_meshopt_compression& compression = source.meshopt_compression;
                const auto compressed_buffer = static_cast<std::size_t>(compression.buffer - data.buffers);
                (*extension)["buffer"] = new_buffer[compressed_buffer];
                const ByteView compressed = document.buffer(compressed_buffer).sub(compression.offset, compression.size);
                if (!compressed.empty()) {
                    (*extension)["byteOffset"] = place(compressed);
                }
            }
        }
        position = alignUp(position, view_alignment);
        buffers[0] = Json{{"byteLength", position}};
        for (const auto& [name, buffer] : fallback_buffer) {
            buffers[buffer]["byteLength"] = fallback_length[name];
        }
    }
    if (buffers.empty()) {
        json.erase("buffers");
    } else {
        json["buffers"] = std::move(buffers);
    }
    return plan;
}

// The input file whose bytes the plan writes, whole and in place, if any: a
// `.bin` whose views are copied unchanged and in order, with zero padding.
// Returns its entry in files() and its mapped bytes.
std::optional<std::pair<std::size_t, ByteView>> identicalFile(const Document& document, const BinPlan& plan) {
    const cgltf_data& data = document.data();
    std::optional<std::size_t> file;
    const std::uint8_t* base = nullptr;
    for (std::size_t i = 0; i < data.buffers_count && !file; ++i) {
        file = document.bufferFile(i);
        base = document.buffer(i).data;
    }
    if (!file || base == nullptr || document.files()[*file].size != plan.length) {
        return std::nullopt;
    }
    std::uint64_t end = 0;
    const auto zeros = [&](std::uint64_t from, std::uint64_t to) {
        return std::all_of(base + from, base + to, [](std::uint8_t byte) { return byte == 0; });
    };
    for (const Piece& piece : plan.pieces) {
        if (piece.bytes.empty()) {
            continue;
        }
        if (piece.bytes.data != base + piece.offset || piece.offset < end) {
            return std::nullopt;
        }
        if (!zeros(end, piece.offset)) {
            return std::nullopt;
        }
        end = piece.offset + piece.bytes.size;
    }
    if (end > plan.length || !zeros(end, plan.length)) {
        return std::nullopt;
    }
    return std::make_pair(*file, ByteView{base, static_cast<std::size_t>(plan.length)});
}

void writePieces(common::OutputFile& file, const BinPlan& plan) {
    const std::uint64_t start = file.position();
    for (const Piece& piece : plan.pieces) {
        file.align(view_alignment);
        if (file.position() - start != piece.offset) {
            throw std::logic_error("the .bin plan and its writing disagree");
        }
        file.write(piece.bytes);
    }
    file.align(view_alignment);
}

} // namespace

std::vector<WrittenFile> writeSeparate(const Document& document, const Layout& layout, const fs::path& directory,
                                       const std::string& stem, common::Outputs& outputs) {
    BinPlan plan = planBin(document, layout);
    Json& json = plan.json;
    std::vector<WrittenFile> written;
    written.push_back({FileRole::gltf, stem + ".gltf", 0});

    if (plan.any_data) {
        const std::string bin_name = stem + ".bin";
        const fs::path destination = directory / fs::u8path(bin_name);
        if (const auto same = identicalFile(document, plan)) {
            // Copied (or linked from a chain's parent stage) without going
            // through the process.
            outputs.copy(document.files()[same->first].path, same->second, destination);
        } else {
            common::OutputFile bin = outputs.create(destination);
            writePieces(bin, plan);
            bin.close();
        }
        json["buffers"][0]["uri"] = encodeUriSegment(bin_name);
        written.push_back({FileRole::buffer, bin_name, plan.length});
    }

    const std::vector<std::string> names = imageFileNames(document, stem);
    std::set<std::string> done;
    for (std::size_t i = 0; i < layout.image_sources.size(); ++i) {
        const std::size_t source = layout.image_sources[i];
        Json& image = json["images"][i];
        image.erase("bufferView");
        if (source == Layout::added_image) {
            // The command wrote the file already.
            const Layout::AddedImage& added = layout.added_images.at(i);
            image["uri"] = encodeUriSegment(added.name);
            written.push_back({FileRole::image, added.name, added.size});
            continue;
        }
        image["uri"] = encodeUriSegment(names[source]);
        // A missing image keeps a reference to its name, with no file behind it.
        const ImageData& data = document.image(source);
        if (!done.insert(names[source]).second || data.missing) {
            continue;
        }
        const fs::path destination = directory / fs::u8path(names[source]);
        if (data.file) {
            outputs.copy(document.files()[*data.file].path, data.bytes, destination);
        } else {
            common::OutputFile file = outputs.create(destination);
            file.write(data.bytes);
            file.close();
        }
        written.push_back({FileRole::image, names[source], data.bytes.size});
    }

    common::OutputFile gltf = outputs.create(directory / fs::u8path(stem + ".gltf"));
    gltf.write(json.dump());
    written.front().size = gltf.position();
    gltf.close();
    return written;
}

namespace {

void writeUint32(common::OutputFile& file, std::uint32_t value) {
    const std::uint8_t bytes[4] = {static_cast<std::uint8_t>(value & 0xFFu), static_cast<std::uint8_t>((value >> 8) & 0xFFu),
                                   static_cast<std::uint8_t>((value >> 16) & 0xFFu),
                                   static_cast<std::uint8_t>((value >> 24) & 0xFFu)};
    file.write(ByteView{bytes, sizeof bytes});
}

const char* mimeFor(const char* extension) {
    const std::string name = extension;
    if (name == "png") {
        return "image/png";
    }
    if (name == "jpg") {
        return "image/jpeg";
    }
    if (name == "webp") {
        return "image/webp";
    }
    if (name == "ktx2") {
        return "image/ktx2";
    }
    return nullptr;
}

} // namespace

std::vector<WrittenFile> writeGlb(const Document& document, const Layout& layout, const fs::path& path,
                                  common::Outputs& outputs) {
    if (!layout.added_images.empty()) {
        // GLB is written from a stage, whose images are all files of its own.
        throw std::logic_error("writeGlb: a layout with added images");
    }
    const std::vector<std::string> names = imageFileNames(document, "model");
    bool any_image = false;
    for (const std::size_t source : layout.image_sources) {
        any_image = any_image || !document.image(source).missing;
    }
    BinPlan plan = planBin(document, layout, any_image);
    Json& json = plan.json;

    // Images follow the views in the BIN chunk, each in a new buffer view;
    // images that share a source file share one.
    std::vector<ByteView> image_bytes;
    std::map<std::string, std::size_t> view_by_name;
    std::uint64_t length = plan.length;
    if (!json.contains("bufferViews") && any_image) {
        json["bufferViews"] = Json::array();
    }
    for (std::size_t i = 0; i < layout.image_sources.size(); ++i) {
        const std::size_t source = layout.image_sources[i];
        const ImageData& data = document.image(source);
        Json& image = json["images"][i];
        image.erase("bufferView");
        if (data.missing) {
            // No bytes to embed: it keeps a reference to its name, as writeSeparate does.
            image["uri"] = encodeUriSegment(names[source]);
            continue;
        }
        image.erase("uri");
        auto found = view_by_name.find(names[source]);
        if (found == view_by_name.end()) {
            length = alignUp(length, view_alignment);
            json["bufferViews"].push_back(Json{{"buffer", 0}, {"byteOffset", length}, {"byteLength", data.bytes.size}});
            found = view_by_name.emplace(names[source], json["bufferViews"].size() - 1).first;
            image_bytes.push_back(data.bytes);
            length += data.bytes.size;
        }
        image["bufferView"] = found->second;
        if (const char* mime = mimeFor(extensionFor(data, image))) {
            image["mimeType"] = mime;
        }
    }
    length = alignUp(length, view_alignment);
    if (plan.any_data) {
        json["buffers"][0] = Json{{"byteLength", length}};
    }

    std::string text = json.dump();
    text.append((4 - text.size() % 4) % 4, ' ');
    const std::uint64_t total = 12 + 8 + text.size() + (plan.any_data ? 8 + length : 0);
    if (total > std::numeric_limits<std::uint32_t>::max()) {
        throw common::Failure(common::ExitCode::input_rejected, "glb-too-large",
                              "the model needs " + std::to_string(total) +
                                  " bytes, more than a GLB file can hold (4 GiB): write a .gltf instead");
    }
    common::OutputFile file = outputs.create(path);
    writeUint32(file, 0x46546C67u); // "glTF"
    writeUint32(file, 2);
    writeUint32(file, static_cast<std::uint32_t>(total));
    writeUint32(file, static_cast<std::uint32_t>(text.size()));
    writeUint32(file, 0x4E4F534Au); // "JSON"
    file.write(text);
    if (plan.any_data) {
        writeUint32(file, static_cast<std::uint32_t>(length));
        writeUint32(file, 0x004E4942u); // "BIN\0"
        const std::uint64_t start = file.position();
        writePieces(file, plan);
        for (const ByteView bytes : image_bytes) {
            file.align(view_alignment);
            file.write(bytes);
        }
        file.align(view_alignment);
        if (file.position() - start != length) {
            throw std::logic_error("the GLB plan and its writing disagree");
        }
    }
    const std::uint64_t size = file.position();
    file.close();
    return {{FileRole::glb, path.filename().u8string(), size}};
}

} // namespace meshtinker::gltf
