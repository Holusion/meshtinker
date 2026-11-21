#pragma once

#include "ktx.hpp"

#include "meshtinker/image/image.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace meshtinker::ktx {

struct EncodeParams {
    Codec codec = Codec::etc1s;
    // sRGB color: perceptual metrics, sRGB mips and transfer function.
    bool srgb = false;
    // A normal map: linear metrics, no RDO, renormalized mips (basisu's -normal_map).
    bool normal = false;
    // Writes RGBA rather than RGB.
    bool alpha = false;
    Settings settings;
};

// basisu's compressor, with a job pool of `threads` threads (the calling one
// included). Several encoders can run at once on different threads.
class Encoder {
public:
    explicit Encoder(unsigned threads);
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;
    Encoder(Encoder&&) = delete;
    Encoder& operator=(Encoder&&) = delete;
    ~Encoder();

    // A KTX2 file of `image`, with a full mip chain. Its sides must be
    // multiples of 4. Empty, with basisu's reason in `error`, if it fails.
    // basisu copies the pixels, and `image` is freed before encoding starts.
    std::optional<std::vector<std::uint8_t>> encode(image::Image image, const EncodeParams& params,
                                                    std::string& error);

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace meshtinker::ktx
