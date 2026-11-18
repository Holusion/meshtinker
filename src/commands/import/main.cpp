// meshtinker-import: rewrites a glTF or GLB file in the normalized profile
// (docs/normalized-profile.md) and reports the statistics of what it wrote.

#include "meshtinker/analysis/analyze.hpp"
#include "meshtinker/analysis/pipeline.hpp"
#include "meshtinker/cli/command.hpp"
#include "meshtinker/common/failure.hpp"
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
    fs::path input;
    fs::path output;
    command.app().add_option("input", input, "The .gltf or .glb file")->required();
    command.app()
        .add_option("output", output, "The .gltf file to write. Its directory must exist; no file is overwritten")
        ->required()
        ->check([](const std::string& value) {
            return fs::u8path(value).extension() == ".gltf" ? std::string() : std::string("OUTPUT must end in .gltf");
        });
    command.addJsonOption();
    command.addTrustedOption();
    command.addWorkspaceOptions("the output's directory");
    return command.run(argc, argv, [&] {
        fs::path directory = output.parent_path();
        if (directory.empty()) {
            directory = ".";
        }
        std::error_code error;
        if (!fs::is_directory(directory, error)) {
            throw common::Failure(common::ExitCode::machine, "output-directory-missing",
                                  "the output's directory doesn't exist");
        }
        common::Workspace workspace(command.tmpDir().value_or(directory), command.keepTmp());
        analysis::Inspection inspection = analysis::inspect(input, {command.trusted()}, workspace, command.diagnostics());
        const gltf::Layout layout = gltf::normalizedLayout(inspection.document, inspection.selection, command.diagnostics());
        const std::string stem = output.stem().u8string();
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
