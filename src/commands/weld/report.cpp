#include "weld.hpp"

#include "meshtinker/common/format.hpp"

#include <iomanip>
#include <ostream>
#include <sstream>

namespace meshtinker::weld {
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
    json["primitives"] = {{"welded", report.welded},
                          {"unchanged", report.unchanged},
                          {"compressed", report.compressed},
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
    out << "Welded " << report.welded << " of " << report.input.primitives << " triangle primitives";
    if (report.compressed > 0) {
        out << ", " << report.compressed << " compressed ones copied";
    }
    if (report.skipped > 0) {
        out << " (" << report.skipped << " other primitives copied)";
    }
    out << '\n';
    out << "Vertices: " << report.input.vertices << " -> " << report.output.vertices << " ("
        << percent(report.output.vertices, report.input.vertices) << ")\n";
    for (const gltf::WrittenFile& file : report.files) {
        out << "  " << file.name << "  " << common::formatBytes(file.size) << '\n';
    }
}

} // namespace meshtinker::weld
