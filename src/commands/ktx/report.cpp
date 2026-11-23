#include "ktx.hpp"

#include <iomanip>
#include <ostream>
#include <sstream>

namespace meshtinker::ktx {
namespace {

using Json = nlohmann::ordered_json;

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
    json["quality"] = qualityName(report.quality);
    json["maxSize"] = report.max_size == 0 ? Json(nullptr) : Json(report.max_size);
    json["minSize"] = report.min_size == 0 ? Json(nullptr) : Json(report.min_size);
    json["threads"] = report.threads;
    Json images = Json::array();
    std::uint64_t before = 0;
    std::uint64_t after = 0;
    for (const EncodedImage& image : report.encoded) {
        images.push_back({{"source", image.name},
                          {"sourceFormat", image.source_format},
                          {"width", image.width},
                          {"height", image.height},
                          {"image", image.image},
                          {"output", image.output},
                          {"outputWidth", image.output_width},
                          {"outputHeight", image.output_height},
                          {"codec", codecName(image.codec)},
                          {"transferFunction", image.srgb ? "sRGB" : "linear"},
                          {"alpha", image.alpha},
                          {"byteSize", {{"before", image.bytes_before}, {"after", image.bytes_after}}},
                          {"seconds", image.seconds}});
        before += image.bytes_before;
        after += image.bytes_after;
    }
    json["images"] = std::move(images);
    json["totals"] = {{"encoded", report.encoded.size()},
                      {"skipped", report.skipped},
                      {"small", report.small.size()},
                      {"byteSize", {{"before", before}, {"after", after}}},
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
    for (const EncodedImage& image : report.encoded) {
        before += image.bytes_before;
        after += image.bytes_after;
    }
    out << "Encoded " << report.encoded.size() << " images at quality " << qualityName(report.quality);
    if (report.skipped > 0) {
        out << ", left " << report.skipped << " as they were";
    }
    if (!report.small.empty()) {
        out << ", left " << report.small.size() << " small ones as they were";
    }
    out << ", " << std::fixed << std::setprecision(1) << report.seconds << " s\n";
    out << "Image bytes: " << before << " -> " << after << " (" << percent(after, before) << ")\n";
    for (const EncodedImage& image : report.encoded) {
        out << "  " << image.name << " " << image.width << "x" << image.height << " -> " << image.output << " "
            << image.output_width << "x" << image.output_height << ", " << codecName(image.codec) << ' '
            << (image.srgb ? "sRGB" : "linear") << (image.alpha ? " with alpha" : "") << ", " << image.bytes_before
            << " -> " << image.bytes_after << " bytes, " << std::setprecision(1) << image.seconds << " s\n";
    }
}

} // namespace meshtinker::ktx
