#pragma once

#include "avif.hpp"

#include "meshtinker/image/image.hpp"
#include "meshtinker/texture/reencode.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace meshtinker::avif {

// libavif with libaom (docs/avif.md#encoder), with up to `threads` threads.
class Encoder final : public texture::Encoder {
public:
    Encoder(unsigned threads, int quality, int effort);

    // An AVIF file of `image` (docs/avif.md#per-slot-settings). Empty, with
    // libavif's reason in `error`, if it fails.
    std::optional<std::vector<std::uint8_t>> encode(image::Image image, const texture::Request& request,
                                                    std::string& error) override;

private:
    unsigned threads_;
    int quality_;
    int effort_;
};

} // namespace meshtinker::avif
