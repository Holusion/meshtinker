#include "meshtinker/texture/reencode.hpp"

#include "meshtinker/common/format.hpp"
#include "meshtinker/common/log.hpp"
#include "meshtinker/gltf/edit.hpp"
#include "meshtinker/gltf/graph.hpp"
#include "meshtinker/gltf/layout.hpp"
#include "meshtinker/gltf/material_slots.hpp"
#include "meshtinker/gltf/selection.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <map>
#include <set>
#include <thread>

namespace meshtinker::texture {

namespace fs = std::filesystem;
using Json = gltf::Json;

namespace {

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }

using gltf::SlotKind;

const Json* child(const Json& object, const char* key) {
    if (!object.is_object()) {
        return nullptr;
    }
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &*found;
}

// What the known slots of every material ask of each texture.
std::map<std::size_t, Use> textureUses(const Json& json) {
    std::map<std::size_t, Use> uses;
    const Json* materials = child(json, "materials");
    if (materials == nullptr || !materials->is_array()) {
        return uses;
    }
    for (const Json& material : *materials) {
        const bool opaque = gltf::isOpaque(material);
        for (const gltf::TextureSlot& slot : gltf::textureSlots(material)) {
            const std::optional<gltf::KnownSlot> known = gltf::knownSlot(slot.path);
            const Json* index = child(*slot.info, "index");
            if (!known || index == nullptr || !index->is_number_unsigned()) {
                continue;
            }
            Use& use = uses[index->get<std::size_t>()];
            use.color = use.color || known->kind == SlotKind::color;
            use.data = use.data || known->kind == SlotKind::data;
            use.normal = use.normal || known->kind == SlotKind::normal;
            use.alpha = use.alpha || known->alpha || (known->alpha_unless_opaque && !opaque);
        }
    }
    return uses;
}

std::optional<std::size_t> sourceOf(const Json& texture, const char* extension) {
    const Json* holder = &texture;
    if (extension != nullptr) {
        const Json* extensions = child(texture, "extensions");
        holder = extensions != nullptr ? child(*extensions, extension) : nullptr;
    }
    const Json* source = holder != nullptr ? child(*holder, "source") : nullptr;
    if (source == nullptr || !source->is_number_unsigned()) {
        return std::nullopt;
    }
    return source->get<std::size_t>();
}

const char* formatName(const gltf::ImageData& data) {
    return data.header ? gltf::mimeType(data.header->format) : "unknown";
}

// The texture source extensions in the order they are looked at, with the
// format each holds.
struct SourceExtension {
    const char* name;
    const char* format;
};
constexpr SourceExtension source_extensions[] = {
    {"EXT_texture_webp", "WebP"}, {"EXT_texture_avif", "AVIF"}, {"KHR_texture_basisu", "KTX2"}};

bool contains(const std::vector<const char*>& names, const char* name) {
    return std::any_of(names.begin(), names.end(), [&](const char* n) { return std::string(n) == name; });
}

// Whether image::decode reads this extension's sources in this build.
bool decodable(const char* extension) {
    const std::string name = extension;
    return (name == "EXT_texture_webp" && image::decodesWebp()) || (name == "EXT_texture_avif" && image::decodesAvif());
}

bool decodable(gltf::ImageFormat format) {
    return format == gltf::ImageFormat::png || format == gltf::ImageFormat::jpeg ||
           (format == gltf::ImageFormat::webp && image::decodesWebp()) ||
           (format == gltf::ImageFormat::avif && image::decodesAvif());
}

// "PNG and JPEG", or with the formats this build decodes besides.
std::string decodedFormats() {
    std::vector<std::string> formats = {"PNG", "JPEG"};
    if (image::decodesWebp()) {
        formats.emplace_back("WebP");
    }
    if (image::decodesAvif()) {
        formats.emplace_back("AVIF");
    }
    std::string text;
    for (std::size_t i = 0; i < formats.size(); ++i) {
        text += (i == 0 ? "" : i + 1 == formats.size() ? " and " : ", ") + formats[i];
    }
    return text;
}

// One image to encode.
struct Job {
    std::size_t source = 0;
    std::vector<std::size_t> textures;
    Request request;
    bool alpha_sampled = false;
    image::Content content = image::Content::srgb;
    // The source's size, from its header.
    image::Size source_size;
    // Filled by run().
    std::optional<std::vector<std::uint8_t>> file;
    std::string error;
    bool decoded = false;
    image::Size size;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int source_channels = 4;
    double seconds = 0;
};

void run(Job& job, const gltf::Document& document, std::uint32_t max_size, std::uint32_t multiple, Encoder& encoder) {
    const Clock::time_point start = Clock::now();
    const gltf::ImageData& data = document.image(job.source);
    std::optional<image::Image> decoded = image::decode(data.bytes, job.error);
    if (!decoded) {
        return;
    }
    job.decoded = true;
    job.width = decoded->width;
    job.height = decoded->height;
    job.source_channels = decoded->source_channels;
    job.request.alpha = job.alpha_sampled && image::analyze(*decoded).alpha;
    job.size = image::fittedSize({decoded->width, decoded->height}, max_size, multiple);
    if (job.size != image::Size{decoded->width, decoded->height}) {
        // Resampled, then the decoded pixels go: one copy at a time.
        image::Image resized = image::resize(*decoded, job.size, job.content, job.request.alpha);
        decoded = std::move(resized);
    }
    job.file = encoder.encode(std::move(*decoded), job.request, job.error);
    job.seconds = secondsSince(start);
}

// Images whose output is under this many texels don't keep every core busy on
// their own, and are encoded several at a time (docs/images.md#memory-and-threads).
constexpr std::uint64_t small_texels = std::uint64_t{1024} * 1024;

void removeUnusedExtension(Json& json, const char* name, const Json& textures) {
    for (const Json& texture : textures) {
        const Json* extensions = child(texture, "extensions");
        if (extensions != nullptr && child(*extensions, name) != nullptr) {
            return;
        }
    }
    for (const char* list : {"extensionsUsed", "extensionsRequired"}) {
        if (!json.contains(list)) {
            continue;
        }
        Json kept = Json::array();
        for (const Json& entry : json[list]) {
            if (!(entry.is_string() && entry.get<std::string>() == name)) {
                kept.push_back(entry);
            }
        }
        if (kept.empty()) {
            json.erase(list);
        } else {
            json[list] = std::move(kept);
        }
    }
}

} // namespace

Result reencode(const fs::path& input, const fs::path& directory, const std::string& stem, const Options& options,
                const Codec& codec, common::Outputs& outputs, common::Diagnostics& diagnostics) {
    Result result;
    result.threads = options.threads != 0 ? options.threads : image::threadCount();

    gltf::Document document = gltf::readNormalized(input, options.read, diagnostics);
    gltf::Layout layout = gltf::normalizedLayout(document, gltf::selectScene(document), diagnostics);
    Json& json = layout.json;
    const std::vector<std::string> source_names = gltf::imageFileNames(document, stem);
    gltf::ImageNames names(document, stem);
    const std::string command = codec.command();

    // Which textures each image would be encoded for, and what they ask of it.
    std::vector<const char*> kept = codec.keptExtensions();
    if (!contains(kept, codec.extension())) {
        kept.push_back(codec.extension());
    }
    std::string undecodable;
    for (const SourceExtension& extension : source_extensions) {
        if (!contains(kept, extension.name) && !decodable(extension.name)) {
            undecodable += std::string(undecodable.empty() ? "" : " or ") + extension.format;
        }
    }
    std::string only_undecodable = "textures with only a ";
    only_undecodable += undecodable;
    only_undecodable += " source are left as they are: ";
    only_undecodable += command;
    only_undecodable += " decodes " + decodedFormats();
    std::string unknown_slots = "images only slots unknown to ";
    unknown_slots += command;
    unknown_slots += " use are left as they are: their transfer function can't be told";
    const std::map<std::size_t, Use> uses = textureUses(json);
    std::map<std::size_t, std::vector<std::size_t>> textures_of;
    std::map<std::size_t, Use> image_uses;
    std::set<std::size_t> skipped_images;
    const auto skip = [&](std::size_t image, const char* code, const std::string& message) {
        if (skipped_images.insert(image).second) {
            ++result.skipped;
            diagnostics.warn(code, message, source_names[layout.image_sources[image]]);
        }
    };
    const Json empty = Json::array();
    const Json& textures = json.contains("textures") ? json["textures"] : empty;
    for (std::size_t t = 0; t < textures.size(); ++t) {
        const Json& texture = textures[t];
        if (std::any_of(kept.begin(), kept.end(), [&](const char* name) { return sourceOf(texture, name).has_value(); })) {
            continue; // Kept as it is: not re-encoded.
        }
        // The core source first, the original where there is one; then the
        // first extension source this build decodes.
        std::optional<std::size_t> source = sourceOf(texture, nullptr);
        std::optional<std::size_t> other;
        for (const SourceExtension& extension : source_extensions) {
            if (source || contains(kept, extension.name)) {
                continue;
            }
            const std::optional<std::size_t> found = sourceOf(texture, extension.name);
            if (found && decodable(extension.name)) {
                source = found;
            } else if (found && !other) {
                other = found;
            }
        }
        if (!source) {
            if (other) {
                skip(*other, "image-format-unsupported", only_undecodable);
            }
            continue;
        }
        const auto use = uses.find(t);
        if (use == uses.end() || !use->second.any()) {
            skip(*source, "image-slot-unknown", unknown_slots);
            continue;
        }
        textures_of[*source].push_back(t);
        Use& image_use = image_uses[*source];
        image_use.color = image_use.color || use->second.color;
        image_use.data = image_use.data || use->second.data;
        image_use.normal = image_use.normal || use->second.normal;
        image_use.alpha = image_use.alpha || use->second.alpha;
    }

    std::vector<Job> jobs;
    for (const auto& [image, texture_list] : textures_of) {
        const gltf::ImageData& data = document.image(layout.image_sources[image]);
        const Use& use = image_uses[image];
        if (data.header && std::max(data.header->width, data.header->height) <= options.min_size) {
            result.small.push_back(source_names[layout.image_sources[image]]);
            continue;
        }
        if (data.header && data.header->format == gltf::ImageFormat::ktx2) {
            skip(image, "image-already-compressed", "images already in KTX2 are left as they are");
            continue;
        }
        if (!data.header || !decodable(data.header->format)) {
            skip(image, "image-format-unsupported", "images other than " + decodedFormats() + " are left as they are");
            continue;
        }
        if (!codec.mixedUse() && use.color && (use.data || use.normal)) {
            skip(image, "image-color-space-conflict",
                 "images used both as color and as data are left as they are: one file can't have both transfer "
                 "functions");
            continue;
        }
        Job job;
        job.source = image;
        job.source_size = {data.header->width, data.header->height};
        job.textures = texture_list;
        job.request.use = use;
        job.alpha_sampled = use.alpha;
        job.content = use.color ? image::Content::srgb : use.normal ? image::Content::normal : image::Content::linear;
        jobs.push_back(std::move(job));
    }

    // Large images one at a time, each using every thread; small ones in
    // batches, one per thread.
    std::vector<Job*> large;
    std::vector<Job*> small;
    for (Job& job : jobs) {
        const image::Size size = image::fittedSize(job.source_size, options.max_size, codec.multiple());
        (static_cast<std::uint64_t>(size.width) * size.height < small_texels ? small : large).push_back(&job);
    }

    std::vector<Encoded> encoded;
    const std::vector<const char*> replaced = codec.replacedExtensions();
    // Each image is logged as it is done, for callers following progress
    // (docs/logs.md), in the order they finish: large ones first.
    std::size_t done = 0;
    const auto finish = [&](Job& job) {
        const std::size_t source = layout.image_sources[job.source];
        const gltf::ImageData& data = document.image(source);
        const std::string& source_name = source_names[source];
        const std::string position = "image " + std::to_string(++done) + " of " + std::to_string(jobs.size());
        if (!job.decoded) {
            common::logger().info("Left " + position + " as it was: " + source_name + " can't be decoded");
            skip(job.source, "image-decode-failed", "images that can't be decoded are left as they are (" + job.error + ")");
            return;
        }
        if (!job.file) {
            common::logger().info("Left " + position + " as it was: " + codec.library() + " can't encode " + source_name);
            skip(job.source, "image-encode-failed",
                 std::string("images ") + codec.library() + " can't encode are left as they are (" + job.error + ")");
            return;
        }
        if (codec.keepsSmallerSource() && job.file->size() >= data.bytes.size) {
            common::logger().info("Kept " + position + " as it was: " + source_name + " is " +
                                  common::formatBytes(data.bytes.size) + ", the new file would be " +
                                  common::formatBytes(job.file->size()));
            result.not_smaller.push_back(source_name);
            job.file.reset();
            return;
        }
        const std::string name = names.reserve(fs::u8path(source_name).replace_extension(codec.fileExtension()).u8string());
        common::OutputFile file = outputs.create(directory / fs::u8path(name));
        file.write({job.file->data(), job.file->size()});
        file.close();
        const std::uint64_t size = job.file->size();
        job.file.reset();

        // The new image keeps the source's name, extras and extensions.
        Json image_json = json["images"][job.source];
        image_json.erase("uri");
        image_json.erase("bufferView");
        image_json["mimeType"] = codec.mimeType();
        const std::size_t added = gltf::addImage(layout, std::move(image_json), name, size);
        for (const std::size_t t : job.textures) {
            Json& texture = json["textures"][t];
            texture.erase("source");
            if (texture.contains("extensions")) {
                for (const char* extension : replaced) {
                    texture["extensions"].erase(extension);
                }
            }
            texture["extensions"][codec.extension()] = Json{{"source", added}};
        }

        Encoded entry;
        entry.source = source;
        entry.name = source_name;
        entry.source_format = formatName(data);
        entry.width = job.width;
        entry.height = job.height;
        entry.source_channels = job.source_channels;
        entry.request = job.request;
        entry.output = name;
        entry.output_width = job.size.width;
        entry.output_height = job.size.height;
        entry.bytes_before = data.bytes.size;
        entry.bytes_after = size;
        entry.seconds = job.seconds;
        common::logger().info("Encoded " + position + ": " + source_name + " " + std::to_string(job.width) + "x" +
                              std::to_string(job.height) + " -> " + name + " " + std::to_string(job.size.width) + "x" +
                              std::to_string(job.size.height) + ", " + common::formatBytes(data.bytes.size) + " -> " +
                              common::formatBytes(size) + " in " + common::formatDuration(job.seconds));
        encoded.push_back(std::move(entry));
    };

    if (!large.empty()) {
        const std::unique_ptr<Encoder> encoder = codec.encoder(result.threads);
        for (Job* job : large) {
            run(*job, document, options.max_size, codec.multiple(), *encoder);
            finish(*job);
            document.releaseMappedPages();
        }
    }
    for (std::size_t first = 0; first < small.size(); first += result.threads) {
        const std::size_t last = std::min(small.size(), first + result.threads);
        std::vector<std::exception_ptr> errors(last - first);
        std::vector<std::thread> workers;
        for (std::size_t i = first; i < last; ++i) {
            workers.emplace_back([&, i] {
                try {
                    const std::unique_ptr<Encoder> encoder = codec.encoder(1);
                    run(*small[i], document, options.max_size, codec.multiple(), *encoder);
                } catch (...) {
                    errors[i - first] = std::current_exception();
                }
            });
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
        for (const std::exception_ptr& error : errors) {
            if (error) {
                std::rethrow_exception(error);
            }
        }
        for (std::size_t i = first; i < last; ++i) {
            finish(*small[i]);
        }
    }

    if (!encoded.empty()) {
        gltf::useExtension(json, codec.extension(), true);
        for (const char* extension : replaced) {
            removeUnusedExtension(json, extension, json["textures"]);
        }
    }
    gltf::prune(layout);
    // The output index of each new image, once prune has renumbered them.
    std::map<std::string, std::size_t> added_index;
    for (const auto& [index, added] : layout.added_images) {
        added_index[added.name] = index;
    }
    for (Encoded& image : encoded) {
        image.image = added_index.at(image.output);
    }
    std::sort(encoded.begin(), encoded.end(), [](const Encoded& a, const Encoded& b) { return a.image < b.image; });
    result.encoded = std::move(encoded);
    result.files = gltf::writeSeparate(document, layout, directory, stem, outputs);
    return result;
}

} // namespace meshtinker::texture
