#include "uv.hpp"

#include "meshtinker/common/format.hpp"

#include <iomanip>
#include <ostream>
#include <sstream>

namespace meshtinker::uv {
namespace {

using Json = nlohmann::ordered_json;

std::string percent(double fraction) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << 100.0 * fraction << '%';
    return out.str();
}

} // namespace

nlohmann::ordered_json toJson(const Report& report, nlohmann::ordered_json header, const common::Diagnostics& diagnostics) {
    Json json = std::move(header);
    Json groups = Json::array();
    std::uint64_t before = 0;
    std::uint64_t after = 0;
    std::uint64_t repacked = 0;
    for (const Group& group : report.groups) {
        Json images = Json::array();
        for (const GroupImage& image : group.images) {
            Json entry = {{"source", image.source}, {"width", image.width}, {"height", image.height}};
            if (!image.output.empty()) {
                entry["output"] = image.output;
                entry["outputWidth"] = image.output_width;
                entry["outputHeight"] = image.output_height;
            }
            images.push_back(std::move(entry));
        }
        Json entry = {{"materials", group.materials},
                      {"status", statusName(group.status)},
                      {"primitives", group.primitives},
                      {"charts", group.charts},
                      {"images", std::move(images)},
                      {"texels", {{"before", group.texels_before}, {"after", group.texels_after}}}};
        if (!group.reason.empty()) {
            entry["reason"] = group.reason;
        }
        if (group.status == Status::repacked) {
            entry["occupancy"] = {{"before", group.occupancy_before}, {"after", group.occupancy_after}};
            ++repacked;
        }
        entry["seconds"] = group.seconds;
        groups.push_back(std::move(entry));
        before += group.texels_before;
        after += group.status == Status::repacked ? group.texels_after : group.texels_before;
    }
    json["groups"] = std::move(groups);
    json["totals"] = {{"groups", report.groups.size()},
                      {"repacked", repacked},
                      {"texels", {{"before", before}, {"after", after}}},
                      {"seconds", report.seconds}};
    Json files = Json::array();
    for (const gltf::WrittenFile& file : report.files) {
        files.push_back({{"name", file.name}, {"byteSize", file.size}});
    }
    json["output"] = {{"files", std::move(files)}};
    json["diagnostics"] = diagnostics.toJson();
    return json;
}

void printText(std::ostream& out, const Report& report) {
    std::uint64_t before = 0;
    std::uint64_t after = 0;
    std::uint64_t repacked = 0;
    for (const Group& group : report.groups) {
        before += group.texels_before;
        after += group.status == Status::repacked ? group.texels_after : group.texels_before;
        repacked += group.status == Status::repacked ? 1 : 0;
    }
    out << "Repacked " << repacked << " of " << report.groups.size() << " material groups, "
        << common::formatDuration(report.seconds) << '\n';
    out << "Texels: " << before << " -> " << after << " ("
        << (before == 0 ? std::string("-") : percent(static_cast<double>(after) / static_cast<double>(before))) << ")\n";
    for (const Group& group : report.groups) {
        out << "  " << group.materials.front();
        if (group.materials.size() > 1) {
            out << " and " << group.materials.size() - 1 << " more";
        }
        out << ": " << statusName(group.status);
        if (!group.reason.empty()) {
            out << " (" << group.reason << ")";
        }
        if (group.status == Status::repacked) {
            out << ", " << group.charts << " charts, occupancy " << percent(group.occupancy_before) << " -> "
                << percent(group.occupancy_after);
        }
        out << '\n';
        for (const GroupImage& image : group.images) {
            if (!image.output.empty()) {
                out << "    " << image.source << " " << image.width << "x" << image.height << " -> " << image.output
                    << " " << image.output_width << "x" << image.output_height << '\n';
            }
        }
    }
}

} // namespace meshtinker::uv
