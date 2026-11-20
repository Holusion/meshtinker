// meshtinker-placeholder: builds a low-poly stand-in for a model, to display
// while the model is processed (docs/placeholder.md).

#include "placeholder.hpp"

#include "meshtinker/cli/command.hpp"

#include <iostream>
#include <string>

namespace meshtinker::placeholder {
namespace {

namespace fs = std::filesystem;

int run(int argc, char** argv) {
    cli::Command command("placeholder", "Build a low-poly stand-in for a glTF or GLB model",
                         "Voxelizes the faces the default scene shows at rest, then writes the outer surface of the "
                         "occupied cells as one untextured mesh, placed to overlay the model. Its size depends on "
                         "the resolution and the model's shape, not on its face count.");
    Options options;
    command.addInput();
    command.addOutput();
    command.app()
        .add_option("--resolution", options.resolution,
                    "Cells along the model's longest axis, " + std::to_string(min_resolution) + " to " +
                        std::to_string(max_resolution) + " (default " + std::to_string(default_resolution) + ")")
        ->type_name("N")
        ->check(CLI::Range(min_resolution, max_resolution).description(""));
    command.addJsonOption();
    command.addTrustedOption();
    command.addWorkspaceOptions();
    return command.run(argc, argv, [&] {
        const fs::path directory = command.outputDirectory();
        options.read.trusted = command.trusted();
        common::Workspace workspace = command.workspace();
        common::Outputs outputs;
        const Report report =
            build(command.input(), directory, command.outputStem(), options, workspace, outputs, command.diagnostics());
        outputs.commit();
        if (command.json()) {
            std::cout << toJson(report, command.jsonHeader(), command.diagnostics()).dump(2) << '\n';
        } else {
            printText(std::cout, report);
        }
        return 0;
    });
}

} // namespace
} // namespace meshtinker::placeholder

int main(int argc, char** argv) { return meshtinker::placeholder::run(argc, argv); }
