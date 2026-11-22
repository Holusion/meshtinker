#pragma once

// Re-encoding the images of a scene's textures into another format, shared by
// `ktx` and `avif` (docs/images.md#commands-that-encode-images): which images
// are encoded and for what, decoding, resizing, scheduling over threads, and
// pointing the textures at the new images.

#include "meshtinker/common/diagnostics.hpp"
#include "meshtinker/common/outputs.hpp"
#include "meshtinker/gltf/document.hpp"
#include "meshtinker/gltf/writer.hpp"
#include "meshtinker/image/image.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace meshtinker::texture {

// What the known material slots that use an image ask of it.
struct Use {
    // sRGB color (base color, emissive...).
    bool color = false;
    // Linear data other than normals (metallic-roughness, occlusion...).
    bool data = false;
    bool normal = false;
    // A slot samples the alpha channel.
    bool alpha = false;
    bool any() const { return color || data || normal; }
};

// One image as the encoder gets it.
struct Request {
    Use use;
    // The image has alpha below 255 that a slot samples.
    bool alpha = false;
};

// Encodes images, on the thread that calls it, with up to the number of
// threads it was made with. Several encoders may run at once on different
// threads.
class Encoder {
public:
    Encoder() = default;
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;
    Encoder(Encoder&&) = delete;
    Encoder& operator=(Encoder&&) = delete;
    virtual ~Encoder() = default;

    // The encoded file of `image`, or nothing with the reason in `error`.
    // `image` is decoded and resized already, and may be freed early.
    virtual std::optional<std::vector<std::uint8_t>> encode(image::Image image, const Request& request,
                                                            std::string& error) = 0;
};

// The output format.
class Codec {
public:
    Codec() = default;
    Codec(const Codec&) = delete;
    Codec& operator=(const Codec&) = delete;
    Codec(Codec&&) = delete;
    Codec& operator=(Codec&&) = delete;
    virtual ~Codec() = default;

    // The command's name, for messages ("ktx").
    virtual const char* command() const = 0;
    // The encoder library's name, for messages ("basisu").
    virtual const char* library() const = 0;
    // The texture extension that points at the new images ("KHR_texture_basisu").
    virtual const char* extension() const = 0;
    virtual const char* mimeType() const = 0;
    // The new files' extension (".ktx2").
    virtual const char* fileExtension() const = 0;
    // Texture extensions whose source is kept as it is: the texture isn't
    // re-encoded. The output extension is always one.
    virtual std::vector<const char*> keptExtensions() const = 0;
    // Texture extensions whose sources are removed once the texture points at
    // a new image.
    virtual std::vector<const char*> replacedExtensions() const = 0;
    // Whether one file can serve an image used both as color and as data.
    virtual bool mixedUse() const = 0;
    // Whether an image whose new file isn't smaller than its source keeps its
    // source.
    virtual bool keepsSmallerSource() const = 0;
    // The longest side's multiple that sizes must keep (4 for block formats).
    virtual std::uint32_t multiple() const = 0;
    virtual std::unique_ptr<Encoder> encoder(unsigned threads) const = 0;
};

struct Options {
    // The longest side of an output image, 0 for no cap (docs/images.md#resizing).
    std::uint32_t max_size = 0;
    // Threads, 0 for every CPU the process may use.
    unsigned threads = 0;
    gltf::ReadOptions read;
};

// One image that was encoded.
struct Encoded {
    // The source image's index and file name.
    std::size_t source = 0;
    std::string name;
    std::string source_format;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // Channels in the source file: 1 (gray), 2 (gray and alpha), 3 or 4.
    int source_channels = 4;
    Request request;
    // The output image's index and file name, and its size in texels.
    std::size_t image = 0;
    std::string output;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    std::uint64_t bytes_before = 0;
    std::uint64_t bytes_after = 0;
    double seconds = 0;
};

struct Result {
    unsigned threads = 1;
    std::vector<Encoded> encoded;
    // Images left as they are, each with its warning.
    std::uint64_t skipped = 0;
    // Images kept because their new file wasn't smaller (Codec::keepsSmallerSource).
    std::vector<std::string> not_smaller;
    std::vector<gltf::WrittenFile> files;
};

// Encodes the images of `input`'s textures with `codec`, points the textures
// at the new images, and writes the model as `<stem>.gltf`, `<stem>.bin` and
// its images in `directory`.
Result reencode(const std::filesystem::path& input, const std::filesystem::path& directory, const std::string& stem,
                const Options& options, const Codec& codec, common::Outputs& outputs,
                common::Diagnostics& diagnostics);

} // namespace meshtinker::texture
