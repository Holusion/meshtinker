// meshtinker-import: rewrites a glTF or GLB file in the normalized profile
// (docs/normalized-profile.md) and reports the statistics of what it wrote.

#include "meshtinker/analysis/analyze.hpp"
#include "meshtinker/analysis/pipeline.hpp"
#include "meshtinker/cli/command.hpp"
#include "meshtinker/gltf/layout.hpp"
#include "meshtinker/gltf/writer.hpp"

#include <algorithm>
#include <iostream>

namespace meshtinker::import {
namespace {

namespace fs = std::filesystem;

// The lossy rounds the source went through before it reached the chain, as
// far as the file tells (docs/chaining.md#stagejson): Draco and
// KHR_mesh_quantization geometry, JPEG, WebP and KTX2 images. A WebP image
// may be lossless; it is recorded anyway.
void recordSourceRounds(chain::Lossy& lossy, const analysis::Report& report) {
    if (report.geometry.draco > 0) {
        lossy.addRound("geometryQuantization",
                       {{"source", "KHR_draco_mesh_compression"}, {"primitives", report.geometry.draco}}, nullptr);
    }
    const std::vector<std::string>& used = report.extensions_used;
    if (std::find(used.begin(), used.end(), "KHR_mesh_quantization") != used.end()) {
        lossy.addRound("geometryQuantization", {{"source", "KHR_mesh_quantization"}}, nullptr);
    }
    for (const analysis::Report::Image& image : report.images) {
        const std::string format = image.mime_type.value_or("");
        if (format == "image/jpeg" || format == "image/webp" || format == "image/ktx2" || format == "image/avif") {
            lossy.addRound("imageEncoding", {{"image", image.index}, {"format", format}}, nullptr);
        }
    }
}

int run(int argc, char** argv) {
    cli::Command command("import", "Rewrite a glTF or GLB file in the normalized profile",
                         "Writes the output .gltf, one .bin beside it and the images as files, keeping only the "
                         "default scene. Geometry and image encodings, unknown extensions and extras are kept. "
                         "Prints the same statistics as `inspect`, measured on what was written. Given a "
                         "directory, creates a chain there, with the model as its first stage.",
                         cli::Kind::constructor);
    command.addInput();
    command.addOutput();
    command.addJsonOption();
    command.addTrustedOption();
    command.addWorkspaceOptions();
    return command.run(argc, argv, [&] {
        const fs::path directory = command.outputDirectory();
        common::Workspace workspace = command.workspace();
        analysis::Inspection inspection = analysis::inspect(command.input(), {command.trusted()}, workspace, command.diagnostics());
        const gltf::Layout layout = gltf::normalizedLayout(inspection.document, inspection.selection, command.diagnostics());
        const std::string stem = command.outputStem();
        const std::vector<gltf::WrittenFile> files =
            gltf::writeSeparate(inspection.document, layout, directory, stem, command.outputs());
        const analysis::Report report = analysis::outputReport(
            inspection.report, layout, files, gltf::imageFileNames(inspection.document, stem));
        if (chain::StageWriter* stage = command.stage()) {
            recordSourceRounds(stage->lossy(), report);
        }
        const nlohmann::ordered_json json = analysis::toJson(report, command.jsonHeader(), command.diagnostics());
        command.publish(json);
        if (command.json()) {
            std::cout << json.dump(2) << '\n';
        } else {
            analysis::printText(std::cout, report);
        }
        return 0;
    });
}

} // namespace
} // namespace meshtinker::import

int main(int argc, char** argv) { return meshtinker::import::run(argc, argv); }
