#include "optimize.hpp"

#include <iomanip>
#include <ostream>

namespace meshtinker::optimize {

nlohmann::ordered_json toJson(const Report& report, nlohmann::ordered_json header, const common::Diagnostics& diagnostics) {
    using Json = nlohmann::ordered_json;
    Json json = std::move(header);
    json["settings"] = {{"preset", presetName(report.settings.preset)},
                        {"quality", qualityName(report.settings.quality)},
                        {"textures", texturesName(report.settings.textures)}};
    json["machine"] = {{"memory", report.machine.memory}};
    Json steps = Json::array();
    // Each step's diagnostics, tagged with the step, after optimize's own.
    Json all = diagnostics.toJson();
    for (const StepRecord& step : report.steps) {
        steps.push_back({{"command", step.command},
                         {"argv", step.arguments},
                         {"exitCode", step.exit_code},
                         {"seconds", step.seconds},
                         {"report", step.report}});
        if (step.report.is_object() && step.report.contains("diagnostics")) {
            for (Json diagnostic : step.report["diagnostics"]) {
                diagnostic["step"] = step.command;
                all.push_back(std::move(diagnostic));
            }
        }
    }
    json["steps"] = std::move(steps);
    json["input"] = {{"byteSize", report.input_bytes}};
    json["output"] = {{"byteSize", report.output_bytes}};
    json["seconds"] = report.seconds;
    json["diagnostics"] = std::move(all);
    return json;
}

void printText(std::ostream& out, const Report& report) {
    out << "Preset " << presetName(report.settings.preset) << ", quality " << qualityName(report.settings.quality)
        << ", textures " << texturesName(report.settings.textures) << '\n';
    for (const StepRecord& step : report.steps) {
        out << "  " << std::left << std::setw(10) << step.command << std::right << std::fixed << std::setprecision(1)
            << std::setw(7) << step.seconds << " s\n";
    }
    out << "Bytes: " << report.input_bytes << " -> " << report.output_bytes << " in " << std::fixed
        << std::setprecision(1) << report.seconds << " s\n";
}

} // namespace meshtinker::optimize
