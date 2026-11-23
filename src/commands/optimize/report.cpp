#include "optimize.hpp"

#include <cmath>
#include <iomanip>
#include <ostream>

namespace meshtinker::optimize {

nlohmann::ordered_json toJson(const Report& report, nlohmann::ordered_json header, const common::Diagnostics& diagnostics) {
    using Json = nlohmann::ordered_json;
    Json json = std::move(header);
    json["settings"] = {{"preset", presetName(report.settings.preset)},
                        {"quality", qualityName(report.settings.quality)}};
    json["machine"] = {{"memory", report.machine.memory}};
    Json steps = Json::array();
    // Each step's diagnostics, tagged with the step, after optimize's own.
    Json all = diagnostics.toJson();
    for (const StepRecord& step : report.steps) {
        steps.push_back({{"command", step.command},
                         {"argv", step.arguments},
                         {"exitCode", step.exit_code},
                         {"signal", step.signal == 0 ? Json(nullptr) : Json(step.signal)},
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
    json["input"] = {{"byteSize", report.input_bytes}, {"gpuBytes", std::llround(report.input_gpu_bytes)}};
    json["output"] = {{"byteSize", report.output_bytes},
                      {"gpuBytes", std::llround(report.output_gpu_bytes)},
                      {"stage", report.exported_stage}};
    json["seconds"] = report.seconds;
    json["diagnostics"] = std::move(all);
    return json;
}

void printText(std::ostream& out, const Report& report) {
    out << "Preset " << presetName(report.settings.preset) << ", quality " << qualityName(report.settings.quality) << '\n';
    for (const StepRecord& step : report.steps) {
        out << "  " << std::left << std::setw(10) << step.command << std::right;
        out << std::fixed << std::setprecision(1) << std::setw(7) << step.seconds << " s";
        if (step.signal != 0 || step.exit_code != 0) {
            out << "  failed, left out";
        }
        out << '\n';
    }
    out << "Bytes: " << report.input_bytes << " -> " << report.output_bytes << " in " << std::fixed
        << std::setprecision(1) << report.seconds << " s\n";
    out << "GPU memory, estimated: " << std::llround(report.input_gpu_bytes / (1024 * 1024)) << " MB -> "
        << std::llround(report.output_gpu_bytes / (1024 * 1024)) << " MB\n";
}

} // namespace meshtinker::optimize
