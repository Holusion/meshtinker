#include "ktx.hpp"

#include "encode.hpp"

#include "meshtinker/gltf/edit.hpp"
#include "meshtinker/gltf/layout.hpp"
#include "meshtinker/gltf/selection.hpp"
#include "meshtinker/image/image.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <thread>

namespace meshtinker::ktx {

namespace fs = std::filesystem;
using Json = gltf::Json;

const char* qualityName(Quality quality) {
    switch (quality) {
    case Quality::highest:
        return "Highest";
    case Quality::high:
        return "High";
    case Quality::medium:
        return "Medium";
    case Quality::low:
        return "Low";
    case Quality::thumb:
        return "Thumb";
    }
    return "";
}

const char* codecName(Codec codec) { return codec == Codec::etc1s ? "ETC1S" : "UASTC"; }

Settings settings(Quality quality) {
    // eCorpus's quality levels for ETC1S; UASTC level 1 at every quality
    // (docs/ktx.md#settings).
    Settings s;
    switch (quality) {
    case Quality::highest:
        s.etc1s_quality = 250;
        break;
    case Quality::high:
        s.etc1s_quality = 240;
        break;
    case Quality::medium:
        s.etc1s_quality = 220;
        break;
    case Quality::low:
        s.etc1s_quality = 180;
        break;
    case Quality::thumb:
        s.etc1s_quality = 128;
        break;
    }
    return s;
}

namespace {

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }

enum class SlotKind : std::uint8_t { color, data, normal };

// A material slot that takes a texture: core glTF's and the Khronos material
// extensions' (docs/ktx.md#codec-per-slot).
struct Slot {
    // The extension the slot is in, or null for core glTF.
    const char* extension;
    // The object the slot is in, inside the material or the extension.
    const char* parent;
    const char* name;
    SlotKind kind;
    // Whether the slot samples the alpha channel. Base color and diffuse do
    // only when the material isn't OPAQUE.
    bool alpha;
    bool alpha_unless_opaque;
};

constexpr Slot slots[] = {
    {nullptr, "pbrMetallicRoughness", "baseColorTexture", SlotKind::color, false, true},
    {nullptr, "pbrMetallicRoughness", "metallicRoughnessTexture", SlotKind::data, false, false},
    {nullptr, nullptr, "normalTexture", SlotKind::normal, false, false},
    {nullptr, nullptr, "occlusionTexture", SlotKind::data, false, false},
    {nullptr, nullptr, "emissiveTexture", SlotKind::color, false, false},
    {"KHR_materials_clearcoat", nullptr, "clearcoatTexture", SlotKind::data, false, false},
    {"KHR_materials_clearcoat", nullptr, "clearcoatRoughnessTexture", SlotKind::data, false, false},
    {"KHR_materials_clearcoat", nullptr, "clearcoatNormalTexture", SlotKind::normal, false, false},
    {"KHR_materials_sheen", nullptr, "sheenColorTexture", SlotKind::color, false, false},
    {"KHR_materials_sheen", nullptr, "sheenRoughnessTexture", SlotKind::data, true, false},
    {"KHR_materials_specular", nullptr, "specularTexture", SlotKind::data, true, false},
    {"KHR_materials_specular", nullptr, "specularColorTexture", SlotKind::color, false, false},
    {"KHR_materials_transmission", nullptr, "transmissionTexture", SlotKind::data, false, false},
    {"KHR_materials_volume", nullptr, "thicknessTexture", SlotKind::data, false, false},
    {"KHR_materials_iridescence", nullptr, "iridescenceTexture", SlotKind::data, false, false},
    {"KHR_materials_iridescence", nullptr, "iridescenceThicknessTexture", SlotKind::data, false, false},
    {"KHR_materials_anisotropy", nullptr, "anisotropyTexture", SlotKind::normal, false, false},
    {"KHR_materials_diffuse_transmission", nullptr, "diffuseTransmissionTexture", SlotKind::data, true, false},
    {"KHR_materials_diffuse_transmission", nullptr, "diffuseTransmissionColorTexture", SlotKind::color, false, false},
    {"KHR_materials_pbrSpecularGlossiness", nullptr, "diffuseTexture", SlotKind::color, false, true},
    {"KHR_materials_pbrSpecularGlossiness", nullptr, "specularGlossinessTexture", SlotKind::color, true, false},
};

const Json* child(const Json& object, const char* key) {
    if (!object.is_object()) {
        return nullptr;
    }
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &*found;
}

// What the known slots of every material ask of each texture.
struct TextureUse {
    bool color = false;
    bool data = false;
    bool normal = false;
    bool alpha = false;
    bool any() const { return color || data || normal; }
};

std::map<std::size_t, TextureUse> textureUses(const Json& json) {
    std::map<std::size_t, TextureUse> uses;
    const Json* materials = child(json, "materials");
    if (materials == nullptr || !materials->is_array()) {
        return uses;
    }
    for (const Json& material : *materials) {
        const Json* mode = child(material, "alphaMode");
        const bool opaque = mode == nullptr || !mode->is_string() || mode->get<std::string>() == "OPAQUE";
        for (const Slot& slot : slots) {
            const Json* base = &material;
            if (slot.extension != nullptr) {
                const Json* extensions = child(material, "extensions");
                base = extensions != nullptr ? child(*extensions, slot.extension) : nullptr;
            }
            if (base != nullptr && slot.parent != nullptr) {
                base = child(*base, slot.parent);
            }
            const Json* info = base != nullptr ? child(*base, slot.name) : nullptr;
            const Json* index = info != nullptr ? child(*info, "index") : nullptr;
            if (index == nullptr || !index->is_number_unsigned()) {
                continue;
            }
            TextureUse& use = uses[index->get<std::size_t>()];
            use.color = use.color || slot.kind == SlotKind::color;
            use.data = use.data || slot.kind == SlotKind::data;
            use.normal = use.normal || slot.kind == SlotKind::normal;
            use.alpha = use.alpha || slot.alpha || (slot.alpha_unless_opaque && !opaque);
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

// One image to encode.
struct Job {
    std::size_t source = 0;
    std::vector<std::size_t> textures;
    EncodeParams params;
    bool alpha_sampled = false;
    image::Content content = image::Content::srgb;
    // The source's size, from its header.
    image::Size source_size;
    std::string name;
    // Filled by run().
    std::optional<std::vector<std::uint8_t>> ktx2;
    std::string error;
    bool decoded = false;
    bool alpha = false;
    image::Size size;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double seconds = 0;
};

void run(Job& job, const gltf::Document& document, std::uint32_t max_size, Encoder& encoder) {
    const Clock::time_point start = Clock::now();
    const gltf::ImageData& data = document.image(job.source);
    std::optional<image::Image> decoded = image::decode(data.bytes, job.error);
    if (!decoded) {
        return;
    }
    job.decoded = true;
    job.width = decoded->width;
    job.height = decoded->height;
    job.alpha = job.alpha_sampled && image::analyze(*decoded).alpha;
    job.params.alpha = job.alpha;
    job.size = image::fittedSize({decoded->width, decoded->height}, max_size, 4);
    if (job.size != image::Size{decoded->width, decoded->height}) {
        // Resampled, then the decoded pixels go: one copy at a time.
        image::Image resized = image::resize(*decoded, job.size, job.content, job.alpha);
        decoded = std::move(resized);
    }
    job.ktx2 = encoder.encode(std::move(*decoded), job.params, job.error);
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

Report ktx(const fs::path& input, const fs::path& directory, const std::string& stem, const Options& options,
           common::Outputs& outputs, common::Diagnostics& diagnostics) {
    const Clock::time_point start = Clock::now();
    Report report;
    report.quality = options.quality;
    report.max_size = options.max_size;
    report.threads = options.threads != 0 ? options.threads : image::threadCount();

    gltf::Document document = gltf::readNormalized(input, options.read, diagnostics);
    gltf::Layout layout = gltf::normalizedLayout(document, gltf::selectScene(document), diagnostics);
    Json& json = layout.json;
    const std::vector<std::string> source_names = gltf::imageFileNames(document, stem);
    gltf::ImageNames names(document, stem);

    // Which textures each image would be encoded for, and what they ask of it.
    const std::map<std::size_t, TextureUse> uses = textureUses(json);
    std::map<std::size_t, std::vector<std::size_t>> textures_of;
    std::map<std::size_t, TextureUse> image_uses;
    std::set<std::size_t> skipped_images;
    const auto skip = [&](std::size_t image, const char* code, const std::string& message) {
        if (skipped_images.insert(image).second) {
            ++report.skipped;
            diagnostics.warn(code, message, source_names[layout.image_sources[image]]);
        }
    };
    const Json empty = Json::array();
    const Json& textures = json.contains("textures") ? json["textures"] : empty;
    for (std::size_t t = 0; t < textures.size(); ++t) {
        const Json& texture = textures[t];
        if (sourceOf(texture, "KHR_texture_basisu")) {
            continue; // Already KTX2: kept, not re-encoded.
        }
        const std::optional<std::size_t> source = sourceOf(texture, nullptr);
        if (!source) {
            std::optional<std::size_t> other = sourceOf(texture, "EXT_texture_webp");
            if (!other) {
                other = sourceOf(texture, "EXT_texture_avif");
            }
            if (other) {
                skip(*other, "image-format-unsupported",
                     "textures with only a WebP or AVIF source are left as they are: ktx decodes PNG and JPEG");
            }
            continue;
        }
        const auto use = uses.find(t);
        if (use == uses.end() || !use->second.any()) {
            skip(*source, "image-slot-unknown",
                 "images only slots unknown to ktx use are left as they are: their transfer function can't be told");
            continue;
        }
        textures_of[*source].push_back(t);
        TextureUse& image_use = image_uses[*source];
        image_use.color = image_use.color || use->second.color;
        image_use.data = image_use.data || use->second.data;
        image_use.normal = image_use.normal || use->second.normal;
        image_use.alpha = image_use.alpha || use->second.alpha;
    }

    const Settings quality_settings = settings(options.quality);
    std::vector<Job> jobs;
    for (const auto& [image, texture_list] : textures_of) {
        const gltf::ImageData& data = document.image(layout.image_sources[image]);
        const TextureUse& use = image_uses[image];
        if (data.missing) {
            skip(image, "image-missing", "images whose file is missing are left as they are");
            continue;
        }
        if (data.header && data.header->format == gltf::ImageFormat::ktx2) {
            skip(image, "image-already-compressed", "images already in KTX2 are left as they are");
            continue;
        }
        if (!data.header || (data.header->format != gltf::ImageFormat::png && data.header->format != gltf::ImageFormat::jpeg)) {
            skip(image, "image-format-unsupported", "images other than PNG and JPEG are left as they are");
            continue;
        }
        if (use.color && (use.data || use.normal)) {
            skip(image, "image-color-space-conflict",
                 "images used both as color and as data are left as they are: one file can't have both transfer "
                 "functions");
            continue;
        }
        Job job;
        job.source = image;
        job.source_size = {data.header->width, data.header->height};
        job.textures = texture_list;
        job.params.settings = quality_settings;
        job.params.codec = use.color ? Codec::etc1s : Codec::uastc;
        job.params.srgb = use.color;
        job.params.normal = use.normal;
        job.alpha_sampled = use.alpha;
        job.content = use.color ? image::Content::srgb : use.normal ? image::Content::normal : image::Content::linear;
        jobs.push_back(std::move(job));
    }

    // Large images one at a time, each using every thread; small ones in
    // batches, one per thread.
    std::vector<Job*> large;
    std::vector<Job*> small;
    for (Job& job : jobs) {
        const image::Size size = image::fittedSize(job.source_size, options.max_size, 4);
        (static_cast<std::uint64_t>(size.width) * size.height < small_texels ? small : large).push_back(&job);
    }

    std::vector<EncodedImage> encoded;
    const auto finish = [&](Job& job) {
        const std::size_t source = layout.image_sources[job.source];
        const gltf::ImageData& data = document.image(source);
        const std::string& source_name = source_names[source];
        if (!job.decoded) {
            skip(job.source, "image-decode-failed", "images that can't be decoded are left as they are (" + job.error + ")");
            return;
        }
        if (!job.ktx2) {
            skip(job.source, "image-encode-failed", "images basisu can't encode are left as they are (" + job.error + ")");
            return;
        }
        const std::string name = names.reserve(fs::u8path(source_name).replace_extension(".ktx2").u8string());
        common::OutputFile file = outputs.create(directory / fs::u8path(name));
        file.write({job.ktx2->data(), job.ktx2->size()});
        file.close();
        const std::uint64_t size = job.ktx2->size();
        job.ktx2.reset();

        // The new image keeps the source's name, extras and extensions.
        Json image_json = json["images"][job.source];
        image_json.erase("uri");
        image_json.erase("bufferView");
        image_json["mimeType"] = "image/ktx2";
        const std::size_t added = gltf::addImage(layout, std::move(image_json), name, size);
        for (const std::size_t t : job.textures) {
            Json& texture = json["textures"][t];
            texture.erase("source");
            if (texture.contains("extensions")) {
                texture["extensions"].erase("EXT_texture_webp");
                texture["extensions"].erase("EXT_texture_avif");
            }
            texture["extensions"]["KHR_texture_basisu"] = Json{{"source", added}};
        }

        EncodedImage result;
        result.source = source;
        result.name = source_name;
        result.source_format = formatName(data);
        result.width = job.width;
        result.height = job.height;
        result.codec = job.params.codec;
        result.srgb = job.params.srgb;
        result.alpha = job.alpha;
        result.output = name;
        result.output_width = job.size.width;
        result.output_height = job.size.height;
        result.bytes_before = data.bytes.size;
        result.bytes_after = size;
        result.seconds = job.seconds;
        encoded.push_back(std::move(result));
    };

    if (!large.empty()) {
        Encoder encoder(report.threads);
        for (Job* job : large) {
            run(*job, document, options.max_size, encoder);
            finish(*job);
            document.releaseMappedPages();
        }
    }
    for (std::size_t first = 0; first < small.size(); first += report.threads) {
        const std::size_t last = std::min(small.size(), first + report.threads);
        std::vector<std::exception_ptr> errors(last - first);
        std::vector<std::thread> workers;
        for (std::size_t i = first; i < last; ++i) {
            workers.emplace_back([&, i] {
                try {
                    Encoder encoder(1);
                    run(*small[i], document, options.max_size, encoder);
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
        gltf::useExtension(json, "KHR_texture_basisu", true);
        removeUnusedExtension(json, "EXT_texture_webp", json["textures"]);
        removeUnusedExtension(json, "EXT_texture_avif", json["textures"]);
    }
    gltf::prune(layout);
    // The output index of each new image, once prune has renumbered them.
    std::map<std::string, std::size_t> added_index;
    for (const auto& [index, added] : layout.added_images) {
        added_index[added.name] = index;
    }
    for (EncodedImage& image : encoded) {
        image.image = added_index.at(image.output);
    }
    std::sort(encoded.begin(), encoded.end(), [](const EncodedImage& a, const EncodedImage& b) { return a.image < b.image; });
    report.encoded = std::move(encoded);
    report.files = gltf::writeSeparate(document, layout, directory, stem, outputs);
    report.seconds = secondsSince(start);
    return report;
}

} // namespace meshtinker::ktx
