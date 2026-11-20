#include "simplify.hpp"

#include <iomanip>
#include <sstream>
#include <ostream>

namespace meshtinker::simplify {
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
    json["error"] = {{"target", report.target_error}, {"achieved", report.achieved_error}};
    json["primitives"] = {{"simplified", report.simplified},
                          {"unchanged", report.unchanged},
                          {"removed", report.removed},
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
    out << "Simplified " << report.simplified << " of " << report.input.primitives << " triangle primitives";
    if (report.removed > 0) {
        out << ", removed " << report.removed;
    }
    if (report.skipped > 0) {
        out << " (" << report.skipped << " other primitives copied)";
    }
    out << ", error " << report.achieved_error << " of " << report.target_error << " allowed\n";
    out << "Faces: " << report.input.faces << " -> " << report.output.faces << " ("
        << percent(report.output.faces, report.input.faces) << ")\n";
    out << "Vertices: " << report.input.vertices << " -> " << report.output.vertices << " ("
        << percent(report.output.vertices, report.input.vertices) << ")\n";
    for (const gltf::WrittenFile& file : report.files) {
        out << "  " << file.name << "  " << file.size << " bytes\n";
    }
}

} // namespace meshtinker::simplify
