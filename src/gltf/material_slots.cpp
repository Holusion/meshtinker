#include "meshtinker/gltf/material_slots.hpp"

#include <cstring>

namespace meshtinker::gltf {

namespace {

struct Entry {
    const char* path;
    KnownSlot slot;
};

constexpr Entry entries[] = {
    {"baseColorTexture", {SlotKind::color, false, true}},
    {"metallicRoughnessTexture", {SlotKind::data, false, false}},
    {"normalTexture", {SlotKind::normal, false, false}},
    {"occlusionTexture", {SlotKind::data, false, false}},
    {"emissiveTexture", {SlotKind::color, false, false}},
    {"KHR_materials_clearcoat.clearcoatTexture", {SlotKind::data, false, false}},
    {"KHR_materials_clearcoat.clearcoatRoughnessTexture", {SlotKind::data, false, false}},
    {"KHR_materials_clearcoat.clearcoatNormalTexture", {SlotKind::normal, false, false}},
    {"KHR_materials_sheen.sheenColorTexture", {SlotKind::color, false, false}},
    {"KHR_materials_sheen.sheenRoughnessTexture", {SlotKind::data, true, false}},
    {"KHR_materials_specular.specularTexture", {SlotKind::data, true, false}},
    {"KHR_materials_specular.specularColorTexture", {SlotKind::color, false, false}},
    {"KHR_materials_transmission.transmissionTexture", {SlotKind::data, false, false}},
    {"KHR_materials_volume.thicknessTexture", {SlotKind::data, false, false}},
    {"KHR_materials_iridescence.iridescenceTexture", {SlotKind::data, false, false}},
    {"KHR_materials_iridescence.iridescenceThicknessTexture", {SlotKind::data, false, false}},
    {"KHR_materials_anisotropy.anisotropyTexture", {SlotKind::normal, false, false}},
    {"KHR_materials_diffuse_transmission.diffuseTransmissionTexture", {SlotKind::data, true, false}},
    {"KHR_materials_diffuse_transmission.diffuseTransmissionColorTexture", {SlotKind::color, false, false}},
    {"KHR_materials_pbrSpecularGlossiness.diffuseTexture", {SlotKind::color, false, true}},
    {"KHR_materials_pbrSpecularGlossiness.specularGlossinessTexture", {SlotKind::color, true, false}},
};

} // namespace

std::optional<KnownSlot> knownSlot(const std::string& path) {
    for (const Entry& entry : entries) {
        if (path == entry.path) {
            return entry.slot;
        }
    }
    return std::nullopt;
}

bool isOpaque(const Json& material) {
    if (!material.is_object()) {
        return true;
    }
    const auto mode = material.find("alphaMode");
    return mode == material.end() || !mode->is_string() || mode->get<std::string>() == "OPAQUE";
}

} // namespace meshtinker::gltf
