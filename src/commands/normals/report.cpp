#include "normals.hpp"

#include <iomanip>
#include <ostream>
#include <sstream>

namespace meshtinker::normals {
namespace {

using Json = nlohmann::ordered_json;

Json countsJson(const Counts& counts) {
    return {{"primitives", counts.primitives}, {"vertices", counts.vertices}, {"faces", counts.faces}};
}

std::string percent(std::uint64_t part, std::uint64_t whole) {
    if (whole == 0) {
        return "-";
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << 100.0 * static_cast<double>(part) / static_cast<double>(whole) << '%';
    return out.str();
}

} // namespace

nlohmann::ordered_json toJson(const Report& report, nlohmann::ordered_json header, const common::Diagnostics& diagnostics) {
    Json json = std::move(header);
    json["creaseAngle"] = report.crease_angle;
    json["primitives"] = {{"added", report.added},
                          {"smoothed", report.smoothed},
                          {"kept", report.kept},
                          {"unlit", report.unlit},
                          {"skipped", report.skipped}};
    json["input"] = countsJson(report.input);
    Json files = Json::array();
    for (const gltf::WrittenFile& file : report.files) {
        files.push_back({{"name", file.name}, {"byteSize", file.size}});
    }
    json["output"] = countsJson(report.output);
    json["output"]["files"] = std::move(files);
    json["diagnostics"] = diagnostics.toJson();
    return json;
}

void printText(std::ostream& out, const Report& report) {
    out << "Wrote normals on " << report.input.primitives << " triangle primitives: " << report.added
        << " without normals, " << report.smoothed << " with flat ones\n";
    out << "Kept " << report.kept << " primitives' own normals";
    if (report.unlit > 0) {
        out << ", " << report.unlit << " unlit primitives";
    }
    if (report.skipped > 0) {
        out << " (" << report.skipped << " other primitives copied)";
    }
    out << '\n';
    if (report.input.primitives > 0) {
        out << "Vertices: " << report.input.vertices << " -> " << report.output.vertices << " ("
            << percent(report.output.vertices, report.input.vertices) << "), crease angle " << report.crease_angle
            << "°\n";
    }
    for (const gltf::WrittenFile& file : report.files) {
        out << "  " << file.name << "  " << file.size << " bytes\n";
    }
}

} // namespace meshtinker::normals
