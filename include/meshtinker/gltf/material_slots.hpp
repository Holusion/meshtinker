#pragma once

// What the material slots of core glTF and the Khronos material extensions
// hold, for the commands that change images (ktx, uv).

#include "meshtinker/gltf/document.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace meshtinker::gltf {

enum class SlotKind : std::uint8_t { color, data, normal };

struct KnownSlot {
    // Color slots hold sRGB data; data and normal slots linear data.
    SlotKind kind;
    // Whether the slot samples the alpha channel. Base color and diffuse do
    // only when the material isn't OPAQUE.
    bool alpha;
    bool alpha_unless_opaque;
};

// The slot at `path`, as gltf::textureSlots names it (`baseColorTexture`,
// `KHR_materials_clearcoat.clearcoatNormalTexture`), or empty for a slot of
// an extension this table doesn't know (docs/ktx.md#codec-per-slot).
std::optional<KnownSlot> knownSlot(const std::string& path);

// Whether `material` is OPAQUE, as it is when alphaMode is missing.
bool isOpaque(const Json& material);

} // namespace meshtinker::gltf
