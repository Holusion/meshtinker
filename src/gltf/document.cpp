#include "meshtinker/gltf/document.hpp"

#include "meshtinker/common/failure.hpp"
#include "meshtinker/common/mapped_file.hpp"
#include "meshtinker/common/references.hpp"
#include "meshtinker/gltf/graph.hpp"
#include "meshtinker/gltf/layout.hpp"
#include "meshtinker/gltf/uri.hpp"

#include <map>
#include <system_error>
#include <utility>

namespace meshtinker::gltf {
namespace fs = std::filesystem;
using common::ByteView;
using common::rejected;

namespace {

struct DataDeleter {
    void operator()(cgltf_data* data) const noexcept { cgltf_free(data); }
};

const char* resultName(cgltf_result result) {
    switch (result) {
    case cgltf_result_success:
        return "success";
    case cgltf_result_data_too_short:
        return "the file is too short";
    case cgltf_result_unknown_format:
        return "unknown file format";
    case cgltf_result_invalid_json:
        return "invalid JSON";
    case cgltf_result_invalid_gltf:
        return "invalid glTF";
    case cgltf_result_invalid_options:
        return "invalid parser options";
    case cgltf_result_file_not_found:
        return "file not found";
    case cgltf_result_io_error:
        return "I/O error";
    case cgltf_result_out_of_memory:
        return "out of memory";
    case cgltf_result_legacy_gltf:
        return "glTF 1.0 is not supported";
    case cgltf_result_max_enum:
        break;
    }
    return "unknown parser error";
}

std::string fileName(const fs::path& path) { return path.filename().u8string(); }

// A GLB header, or JSON whose first non-blank character opens an object.
// cgltf calls anything else invalid JSON, which is misleading for an image or
// an OBJ given by mistake.
bool looksLikeGltf(ByteView bytes) {
    if (bytes.size >= 4 && bytes.data[0] == 'g' && bytes.data[1] == 'l' && bytes.data[2] == 'T' && bytes.data[3] == 'F') {
        return true;
    }
    std::size_t i = 0;
    // A UTF-8 byte order mark, which the spec forbids but some exporters write.
    if (bytes.size >= 3 && bytes.data[0] == 0xEF && bytes.data[1] == 0xBB && bytes.data[2] == 0xBF) {
        i = 3;
    }
    for (; i < bytes.size; ++i) {
        const std::uint8_t c = bytes.data[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            return c == '{';
        }
    }
    return false;
}

// glTF 2.0 files state `asset.version` "2.x" and, optionally, a minVersion
// this reader must support: only 2.0 exists.
bool isGltf2(const Json& json) {
    const auto asset = json.find("asset");
    if (asset == json.end() || !asset->is_object()) {
        return false;
    }
    const auto version = asset->find("version");
    if (version == asset->end() || !version->is_string() || version->get<std::string>().rfind("2.", 0) != 0) {
        return false;
    }
    const auto min_version = asset->find("minVersion");
    return min_version == asset->end() || (min_version->is_string() && min_version->get<std::string>() == "2.0");
}

} // namespace

struct Document::State {
    fs::path path;
    Container container = Container::gltf;
    std::vector<InputFile> files;
    // One mapping per entry in files, in the same order.
    std::vector<common::MappedFile> mappings;
    // Decoded data URIs, which buffers and images point into.
    std::vector<std::vector<std::uint8_t>> decoded;
    std::vector<ImageData> images;
    // The entry in files for each buffer read from a file.
    std::vector<std::optional<std::size_t>> buffer_files;
    Json json;
    // The JSON text cgltf parsed, for a normalized document: cgltf keeps
    // pointers into it (extras offsets).
    std::string parsed_text;
    std::unique_ptr<cgltf_data, DataDeleter> data;

    // Maps the file `reference` leads to, once, and returns its index in files.
    std::size_t addFile(const common::References& references, FileRole role, const std::string& reference,
                        const std::string& what, std::map<fs::path, std::size_t>& by_path) {
        const std::optional<fs::path> resolved = references.resolve(path.parent_path(), reference);
        if (!resolved) {
            throw rejected("reference-refused",
                           what + " '" + reference + "': " + common::refused_reference_message);
        }
        if (const auto found = by_path.find(*resolved); found != by_path.end()) {
            return found->second;
        }
        common::MappedFile mapping = common::MappedFile::open(*resolved, common::Side::input, "'" + reference + "'");
        files.push_back({role, reference, *resolved, mapping.size()});
        mappings.push_back(std::move(mapping));
        by_path.emplace(*resolved, files.size() - 1);
        return files.size() - 1;
    }

    void loadBuffers(const common::References& references, std::map<fs::path, std::size_t>& by_path) {
        cgltf_data& model = *data;
        buffer_files.assign(model.buffers_count, std::nullopt);
        for (std::size_t i = 0; i < model.buffers_count; ++i) {
            cgltf_buffer& buffer = model.buffers[i];
            const std::string what = "buffer " + std::to_string(i);
            ByteView bytes;
            if (buffer.uri == nullptr) {
                if (i == 0 && model.bin != nullptr) {
                    bytes = {static_cast<const std::uint8_t*>(model.bin), model.bin_size};
                } else if (isFallbackBuffer(json["buffers"][i])) {
                    continue;
                } else {
                    throw rejected("buffer-missing", what + " has no uri and no data");
                }
            } else if (isDataUri(buffer.uri)) {
                const std::optional<DataUri> data_uri = parseDataUri(buffer.uri);
                std::optional<std::vector<std::uint8_t>> decoded_bytes;
                if (data_uri) {
                    decoded_bytes = decodeBase64(data_uri->base64);
                }
                if (!decoded_bytes) {
                    throw rejected("invalid-data-uri", what + " has a data URI that isn't base64");
                }
                decoded.push_back(std::move(*decoded_bytes));
                bytes = {decoded.back().data(), decoded.back().size()};
            } else {
                const std::string reference = decodeUri(buffer.uri);
                const std::size_t file = addFile(references, FileRole::buffer, reference, what, by_path);
                bytes = mappings[file].bytes();
                buffer_files[i] = file;
            }
            if (bytes.size < buffer.size) {
                throw rejected("buffer-too-short", what + " holds " + std::to_string(bytes.size) +
                                                       " bytes, less than its byteLength of " +
                                                       std::to_string(buffer.size));
            }
            buffer.data = const_cast<std::uint8_t*>(bytes.data);
            buffer.data_free_method = cgltf_data_free_method_none;
        }
    }

    void loadImages(const common::References& references, std::map<fs::path, std::size_t>& by_path) {
        const cgltf_data& model = *data;
        images.reserve(model.images_count);
        for (std::size_t i = 0; i < model.images_count; ++i) {
            const cgltf_image& image = model.images[i];
            const std::string what = "image " + std::to_string(i);
            ImageData result;
            if (image.buffer_view != nullptr) {
                result.origin = ImageOrigin::buffer_view;
                const auto view_index = static_cast<std::size_t>(image.buffer_view - model.buffer_views);
                result.bytes = viewBytes(view_index);
            } else if (image.uri != nullptr && isDataUri(image.uri)) {
                result.origin = ImageOrigin::data_uri;
                const std::optional<DataUri> data_uri = parseDataUri(image.uri);
                std::optional<std::vector<std::uint8_t>> decoded_bytes;
                if (data_uri) {
                    decoded_bytes = decodeBase64(data_uri->base64);
                }
                if (!decoded_bytes) {
                    throw rejected("invalid-data-uri", what + " has a data URI that isn't base64");
                }
                decoded.push_back(std::move(*decoded_bytes));
                result.bytes = {decoded.back().data(), decoded.back().size()};
            } else if (image.uri != nullptr) {
                result.reference = decodeUri(image.uri);
                if (!references.resolve(path.parent_path(), result.reference)) {
                    result.missing = true;
                    images.push_back(result);
                    continue;
                }
                const std::size_t file = addFile(references, FileRole::image, result.reference, what, by_path);
                result.file = file;
                result.bytes = mappings[file].bytes();
            } else {
                throw rejected("invalid-gltf", what + " has neither a uri nor a bufferView");
            }
            result.header = readImageHeader(result.bytes);
            images.push_back(result);
        }
    }

    ByteView viewBytes(std::size_t index) const {
        const cgltf_buffer_view& view = data->buffer_views[index];
        if (view.buffer == nullptr || view.buffer->data == nullptr) {
            return {};
        }
        const ByteView buffer{static_cast<const std::uint8_t*>(view.buffer->data), view.buffer->size};
        return buffer.sub(view.offset, view.size);
    }
};

Document::Document(std::unique_ptr<State> state) : state_(std::move(state)) {}
Document::Document(Document&& other) noexcept = default;
Document& Document::operator=(Document&& other) noexcept = default;
Document::~Document() = default;

Document Document::read(const fs::path& path, const ReadOptions& options) {
    auto state = std::make_unique<State>();
    state->path = path;
    const std::string name = fileName(path);
    common::MappedFile main = common::MappedFile::open(path, common::Side::input, "'" + name + "'");
    const ByteView bytes = main.bytes();
    if (!looksLikeGltf(bytes)) {
        throw rejected("not-gltf", "'" + name + "' is neither glTF nor GLB");
    }

    cgltf_options parse_options{};
    cgltf_data* raw = nullptr;
    const cgltf_result parsed = cgltf_parse(&parse_options, bytes.data, bytes.size, &raw);
    state->data.reset(raw);
    if (parsed != cgltf_result_success) {
        throw rejected(parsed == cgltf_result_unknown_format ? "not-gltf" : "invalid-gltf",
                       "cannot read '" + name + "': " + resultName(parsed));
    }
    cgltf_data& model = *state->data;
    state->container = model.file_type == cgltf_file_type_glb ? Container::glb : Container::gltf;
    state->json = Json::parse(model.json, model.json + model.json_size, nullptr, false);
    if (state->json.is_discarded() || !state->json.is_object()) {
        throw rejected("invalid-json", "cannot read '" + name + "': invalid JSON");
    }
    if (!isGltf2(state->json)) {
        throw rejected("invalid-gltf", "'" + name + "' is not glTF 2.0: asset.version is missing or not 2.x");
    }
    state->files.push_back({state->container == Container::glb ? FileRole::glb : FileRole::gltf, name, path,
                            main.size()});
    state->mappings.push_back(std::move(main));

    const common::References references = common::References::forInput(path.parent_path(), options.trusted);
    std::map<fs::path, std::size_t> by_path;
    std::error_code error;
    by_path.emplace(fs::canonical(path, error), 0);
    state->loadBuffers(references, by_path);

    const cgltf_result valid = cgltf_validate(&model);
    if (valid != cgltf_result_success) {
        throw rejected("invalid-gltf", "'" + name + "' is not valid glTF: an index, count or range is out of bounds");
    }
    state->loadImages(references, by_path);
    return Document(std::move(state));
}

Document Document::normalized(Document&& source_document, const Layout& layout) {
    const std::unique_ptr<State> source = std::move(source_document.state_);
    const cgltf_data& source_data = *source->data;
    const std::string name = fileName(source->path);

    Json json = layout.json;
    // The layout leaves buffer views naming source buffers, for the writer.
    const std::vector<std::size_t>& buffer_remap = layout.remap[static_cast<std::size_t>(Kind::buffer)];
    std::vector<std::size_t> buffer_sources;
    for (std::size_t i = 0; i < buffer_remap.size(); ++i) {
        if (buffer_remap[i] != Layout::dropped) {
            buffer_sources.push_back(i);
        }
    }
    if (json.contains("bufferViews")) {
        for (Json& view : json["bufferViews"]) {
            forEachReference(Kind::buffer_view, view, [&](Kind target, Json& value) {
                if (target == Kind::buffer && value.is_number_unsigned() &&
                    value.get<std::size_t>() < buffer_remap.size()) {
                    value = buffer_remap[value.get<std::size_t>()];
                }
            });
        }
    }
    if (json.contains("buffers")) {
        for (Json& buffer : json["buffers"]) {
            buffer.erase("uri");
        }
    }
    std::vector<ImageData> images;
    images.reserve(layout.image_sources.size());
    for (std::size_t i = 0; i < layout.image_sources.size(); ++i) {
        const ImageData& image = source->images[layout.image_sources[i]];
        if (image.origin != ImageOrigin::file) {
            json["images"][i].erase("uri");
            json["images"][i].erase("bufferView");
        }
        images.push_back(image);
    }

    auto state = std::make_unique<State>();
    state->path = source->path;
    state->container = source->container;
    state->files = std::move(source->files);
    state->mappings = std::move(source->mappings);
    state->decoded = std::move(source->decoded);
    state->images = std::move(images);
    state->buffer_files.reserve(buffer_sources.size());
    for (const std::size_t buffer : buffer_sources) {
        state->buffer_files.push_back(buffer < source->buffer_files.size() ? source->buffer_files[buffer] : std::nullopt);
    }
    state->parsed_text = json.dump();
    state->json = std::move(json);

    cgltf_options parse_options{};
    cgltf_data* raw = nullptr;
    const cgltf_result parsed =
        cgltf_parse(&parse_options, state->parsed_text.data(), state->parsed_text.size(), &raw);
    state->data.reset(raw);
    if (parsed != cgltf_result_success) {
        throw common::Failure(common::ExitCode::internal, "internal-error",
                              "cannot read the normalized layout of '" + name + "': " + resultName(parsed));
    }
    cgltf_data& model = *state->data;
    for (std::size_t i = 0; i < model.buffers_count; ++i) {
        model.buffers[i].data = source_data.buffers[buffer_sources[i]].data;
        model.buffers[i].data_free_method = cgltf_data_free_method_none;
    }
    if (cgltf_validate(&model) != cgltf_result_success) {
        throw common::Failure(common::ExitCode::internal, "internal-error",
                              "the normalized layout of '" + name + "' is not valid glTF");
    }
    return Document(std::move(state));
}

const cgltf_data& Document::data() const { return *state_->data; }
const Json& Document::json() const { return state_->json; }
Container Document::container() const { return state_->container; }
const fs::path& Document::path() const { return state_->path; }
const std::vector<InputFile>& Document::files() const { return state_->files; }

std::optional<std::size_t> Document::bufferFile(std::size_t index) const {
    return index < state_->buffer_files.size() ? state_->buffer_files[index] : std::nullopt;
}

ByteView Document::buffer(std::size_t index) const {
    const cgltf_buffer& buffer = state_->data->buffers[index];
    if (buffer.data == nullptr) {
        return {};
    }
    return {static_cast<const std::uint8_t*>(buffer.data), buffer.size};
}

ByteView Document::bufferView(std::size_t index) const { return state_->viewBytes(index); }

const ImageData& Document::image(std::size_t index) const { return state_->images[index]; }

void Document::setDecodedView(std::size_t index, const std::uint8_t* bytes) {
    state_->data->buffer_views[index].data = const_cast<std::uint8_t*>(bytes);
}

Document::AccessorSource Document::setAccessorSource(std::size_t index, const AccessorSource& source) {
    cgltf_accessor& accessor = state_->data->accessors[index];
    const AccessorSource previous{accessor.buffer_view, accessor.offset, accessor.stride};
    accessor.buffer_view = const_cast<cgltf_buffer_view*>(source.view);
    accessor.offset = source.offset;
    accessor.stride = source.stride;
    return previous;
}

void Document::releaseMappedPages() const {
    for (const common::MappedFile& mapping : state_->mappings) {
        mapping.releasePages();
    }
}

bool isFallbackBuffer(const Json& buffer) {
    const auto extensions = buffer.find("extensions");
    if (extensions == buffer.end() || !extensions->is_object()) {
        return false;
    }
    for (const char* name : {"EXT_meshopt_compression", "KHR_meshopt_compression"}) {
        const auto extension = extensions->find(name);
        if (extension != extensions->end() && extension->is_object()) {
            const auto fallback = extension->find("fallback");
            if (fallback != extension->end() && fallback->is_boolean() && fallback->get<bool>()) {
                return true;
            }
        }
    }
    return false;
}

} // namespace meshtinker::gltf
