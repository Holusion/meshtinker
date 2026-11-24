#include "dedup.hpp"

#include "meshtinker/common/format.hpp"

#include <ostream>

namespace meshtinker::dedup {
namespace {

using Json = nlohmann::ordered_json;

Json countJson(const Count& count) { return {{"before", count.before}, {"after", count.after}}; }

} // namespace

nlohmann::ordered_json toJson(const Report& report, nlohmann::ordered_json header, const common::Diagnostics& diagnostics) {
    Json json = std::move(header);
    json["images"] = countJson(report.images);
    json["samplers"] = countJson(report.samplers);
    json["textures"] = countJson(report.textures);
    json["materials"] = countJson(report.materials);
    json["accessors"] = countJson(report.accessors);
    json["meshes"] = countJson(report.meshes);
    json["bytes"] = {{"bufferViews", countJson(report.view_bytes)}, {"images", countJson(report.image_bytes)}};
    Json files = Json::array();
    for (const gltf::WrittenFile& file : report.files) {
        files.push_back({{"name", file.name}, {"byteSize", file.size}});
    }
    json["output"] = {{"files", std::move(files)}};
    json["diagnostics"] = diagnostics.toJson();
    return json;
}

void printText(std::ostream& out, const Report& report) {
    const std::pair<const char*, const Count*> rows[] = {
        {"images", &report.images},       {"samplers", &report.samplers},   {"textures", &report.textures},
        {"materials", &report.materials}, {"accessors", &report.accessors}, {"meshes", &report.meshes},
    };
    out << "Merged:";
    const char* separator = " ";
    for (const auto& [name, count] : rows) {
        out << separator << count->before - count->after << " of " << count->before << ' ' << name;
        separator = ", ";
    }
    out << '\n';
    out << "Buffer views: " << common::formatBytes(report.view_bytes.before) << " -> "
        << common::formatBytes(report.view_bytes.after) << "; images: " << common::formatBytes(report.image_bytes.before)
        << " -> " << common::formatBytes(report.image_bytes.after) << '\n';
    for (const gltf::WrittenFile& file : report.files) {
        out << "  " << file.name << "  " << common::formatBytes(file.size) << '\n';
    }
}

} // namespace meshtinker::dedup
