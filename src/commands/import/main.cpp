// meshtinker-import: rewrites a glTF or GLB file in the normalized profile
// (docs/normalized-profile.md) and reports the statistics of what it wrote.

#include "meshtinker/analysis/analyze.hpp"
#include "meshtinker/analysis/pipeline.hpp"
#include "meshtinker/cli/command.hpp"
#include "meshtinker/common/outputs.hpp"
#include "meshtinker/gltf/layout.hpp"
#include "meshtinker/gltf/writer.hpp"

#include <iostream>

namespace meshtinker::import {
namespace {

namespace fs = std::filesystem;

int run(int argc, char** argv) {
    cli::Command command("import", "Rewrite a glTF or GLB file in the normalized profile",
                         "Writes the output .gltf, one .bin beside it and the images as files, keeping only the "
                         "default scene. Geometry and image encodings, unknown extensions and extras are kept. "
                         "Prints the same statistics as `inspect`, measured on what was written.");
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
        common::Outputs outputs;
        const std::vector<gltf::WrittenFile> files =
            gltf::writeSeparate(inspection.document, layout, directory, stem, outputs);
        outputs.commit();
        const analysis::Report report = analysis::outputReport(
            inspection.report, layout, files, gltf::imageFileNames(inspection.document, stem));
        if (command.json()) {
            std::cout << analysis::toJson(report, command.jsonHeader(), command.diagnostics()).dump(2) << '\n';
        } else {
            analysis::printText(std::cout, report);
        }
        return 0;
    });
}

} // namespace
} // namespace meshtinker::import

int main(int argc, char** argv) { return meshtinker::import::run(argc, argv); }
