#include "compress.hpp"

#include "meshtinker/common/format.hpp"

#include <iomanip>
#include <ostream>
#include <sstream>

namespace meshtinker::compress {

nlohmann::ordered_json toJson(const Report& report, nlohmann::ordered_json header, const common::Diagnostics& diagnostics) {
    nlohmann::ordered_json json = std::move(header);
    json["primitives"] = {{"compressed", report.compressed},
                          {"keptDraco", report.kept_draco},
                          {"keptMeshopt", report.kept_meshopt},
                          {"keptUnreadable", report.kept_unreadable}};
    json["meshes"] = {{"quantizedPositions", report.quantized_meshes},
                      {"floatPositions", report.float_meshes},
                      {"addedNodes", report.added_nodes}};
    json["geometry"] = {{"inputBytes", report.input_bytes}, {"outputBytes", report.output_bytes}};
    nlohmann::ordered_json files = nlohmann::ordered_json::array();
    for (const gltf::WrittenFile& file : report.files) {
        files.push_back({{"name", file.name}, {"byteSize", file.size}});
    }
    json["output"] = {{"files", std::move(files)}};
    json["diagnostics"] = diagnostics.toJson();
    return json;
}

void printText(std::ostream& out, const Report& report) {
    out << "Compressed " << report.compressed << " primitives";
    const std::uint64_t kept = report.kept_draco + report.kept_meshopt + report.kept_unreadable;
    if (kept > 0) {
        out << ", kept " << kept << " as they were";
    }
    out << '\n';
    out << "Geometry: " << common::formatBytes(report.input_bytes) << " -> " << common::formatBytes(report.output_bytes);
    if (report.input_bytes > 0) {
        std::ostringstream ratio;
        ratio << std::fixed << std::setprecision(1)
              << 100.0 * static_cast<double>(report.output_bytes) / static_cast<double>(report.input_bytes) << '%';
        out << " (" << ratio.str() << ")";
    }
    out << '\n';
    out << "Positions: " << report.quantized_meshes << " meshes quantized, " << report.float_meshes << " kept float\n";
    for (const gltf::WrittenFile& file : report.files) {
        out << "  " << file.name << "  " << common::formatBytes(file.size) << '\n';
    }
}

} // namespace meshtinker::compress
