#pragma once

#include "meshtinker/common/bytes.hpp"
#include "meshtinker/gltf/image_header.hpp"

#include <cgltf.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace meshtinker::gltf {

// The glTF JSON, with object keys kept in their source order.
using Json = nlohmann::ordered_json;

struct Layout;

enum class Container : std::uint8_t { gltf, glb };

enum class FileRole : std::uint8_t { gltf, glb, buffer, image };

// A file the model consists of: the model file itself, then every external
// buffer and image file, each listed once even if referenced several times.
struct InputFile {
    FileRole role;
    // The reference as written, decoded; the model file's own name for the first entry.
    std::string name;
    std::filesystem::path path;
    std::uint64_t size;
};

enum class ImageOrigin : std::uint8_t { file, buffer_view, data_uri };

struct ImageData {
    ImageOrigin origin = ImageOrigin::file;
    // The encoded image. Empty when it comes from a buffer view that holds no data.
    common::ByteView bytes;
    // The entry in Document::files() for file images.
    std::optional<std::size_t> file;
    // For file images: the `uri` as written, decoded.
    std::string reference;
    // For file images: where `reference` points, relative to the input's
    // folder (common::References::locate). Empty when its text is refused.
    std::string path;
    // A file image whose file is missing, or that confinement refuses: it has
    // no bytes. Without data (ReadOptions::data), only a refused one. Whether that rejects the model depends on how the image is
    // used, which the analysis decides.
    bool missing = false;
    // Empty when the bytes are not a recognized image format.
    std::optional<ImageHeader> header;
};

struct ReadOptions {
    // Allow references outside the model's folder (`--trusted`).
    bool trusted = false;
    // Map the buffer and image files, and decode data URIs. Without, only the
    // model file is read (`inspect --no-data`): references are checked from
    // their text, and buffers, buffer views and images hold no bytes.
    bool data = true;
};

// A parsed glTF or GLB file and the data it references.
//
// The model file and every external buffer and image file are memory-mapped
// read-only (common::MappedFile). GLB chunks and buffer-view images are views
// into those mappings. Only data URIs are decoded to the heap: their text is
// already held by the parsed JSON, which is the same order of size.
//
// Two parses coexist: cgltf's typed view (data()), used to read the model, and
// the JSON as nlohmann values (json()), passed through when writing so that
// unknown extensions and extras survive. Their arrays have the same indices.
//
// Reading fails with a common::Failure when the file isn't glTF, a buffer
// reference is refused or missing, or a buffer is shorter than its byteLength.
// A refused or missing image file is recorded as ImageData::missing.
class Document {
public:
    static Document read(const std::filesystem::path& path, const ReadOptions& options = {});

    // The document `layout` describes, over `source`'s data, which it takes
    // over: its JSON and cgltf view use the layout's indices only, and nothing
    // is copied. Buffers carry no `uri` (their bytes are the source's), and an
    // image read from a buffer view or a data URI has neither `uri` nor
    // `bufferView`: image(i) holds its bytes. path(), container() and files()
    // stay the source's. See readNormalized (layout.hpp).
    static Document normalized(Document&& source, const Layout& layout);

    Document(Document&& other) noexcept;
    Document& operator=(Document&& other) noexcept;
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    ~Document();

    const cgltf_data& data() const;
    const Json& json() const;
    Container container() const;
    const std::filesystem::path& path() const;
    const std::vector<InputFile>& files() const;
    // Whether buffers and images were read (ReadOptions::data).
    bool hasData() const;

    // The bytes of buffer `index`. Empty for a buffer that holds no data, such
    // as a meshopt fallback buffer.
    common::ByteView buffer(std::size_t index) const;
    // The entry in files() that buffer `index` was read from, whole: none for
    // a GLB chunk, a data URI or a buffer without data.
    std::optional<std::size_t> bufferFile(std::size_t index) const;
    // Where buffer `index`'s `uri` points, relative to the input's folder
    // (common::References::locate): empty for a GLB chunk, a data URI or a
    // buffer without data.
    std::string bufferPath(std::size_t index) const;
    // The bytes of buffer view `index`. Empty if its buffer holds no data.
    common::ByteView bufferView(std::size_t index) const;
    const ImageData& image(std::size_t index) const;

    // Hints that mapped pages read so far are no longer needed (see MappedFile).
    void releaseMappedPages() const;

    // Makes cgltf read buffer view `index` from `bytes`, its decoded content
    // (cgltf_buffer_view::data), instead of its buffer. The bytes must outlive
    // the document's use. bufferView() still returns the stored bytes.
    void setDecodedView(std::size_t index, const std::uint8_t* bytes);

    // Where cgltf reads an accessor's elements from.
    struct AccessorSource {
        const cgltf_buffer_view* view = nullptr;
        std::size_t offset = 0;
        std::size_t stride = 0;
    };
    // Makes cgltf read accessor `index` from `source`, such as a buffer view
    // holding decoded Draco data. Returns the previous source, to restore it.
    AccessorSource setAccessorSource(std::size_t index, const AccessorSource& source);

private:
    struct State;
    explicit Document(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

// True if the buffer holds no data by design: it declares the
// EXT_meshopt_compression or KHR_meshopt_compression `fallback` flag.
bool isFallbackBuffer(const Json& buffer);

} // namespace meshtinker::gltf
