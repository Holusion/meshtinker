// meshtinker-inspect: statistics about a glTF or GLB file (docs/inspect-report.md).

#include "meshtinker/analysis/pipeline.hpp"
#include "meshtinker/cli/command.hpp"

#include <iostream>

namespace meshtinker::inspect {
namespace {

int run(int argc, char** argv) {
    cli::Command command("inspect", "Report statistics about a glTF or GLB file",
                         "Sizes, geometry, bounds, images and texture occupancy of the scene the file shows. "
                         "Only the default scene counts, as `import` keeps only "
                         "that one.");
    command.addInput();
    command.addJsonOption();
    command.addTrustedOption();
    command.addWorkspaceOptions();
    return command.run(argc, argv, [&] {
        common::Workspace workspace = command.workspace();
        const analysis::Inspection inspection =
            analysis::inspect(command.input(), {command.trusted()}, workspace, command.diagnostics());
        if (command.json()) {
            std::cout << analysis::toJson(inspection.report, command.jsonHeader(), command.diagnostics()).dump(2) << '\n';
        } else {
            analysis::printText(std::cout, inspection.report);
        }
        return 0;
    });
}

} // namespace
} // namespace meshtinker::inspect

int main(int argc, char** argv) { return meshtinker::inspect::run(argc, argv); }
