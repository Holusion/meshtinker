// meshtinker-compress: quantizes and compresses a model's geometry with
// EXT_meshopt_compression (docs/compress.md).

#include "compress.hpp"

#include "meshtinker/cli/command.hpp"

#include <iostream>

namespace meshtinker::compress {
namespace {

int run(int argc, char** argv) {
    cli::Command command("compress", "Compress the geometry of a glTF or GLB model with meshopt",
                         "Reorders, quantizes and encodes every primitive with EXT_meshopt_compression, as "
                         "glTF-Transform's meshopt \"high\" level does: 14-bit positions, 8-bit octahedral normals, "
                         "12-bit texture coordinates. Draco and meshopt primitives are copied as they are.");
    Options options;
    command.addInput();
    command.addOutput();
    command.addJsonOption();
    command.addTrustedOption();
    command.addWorkspaceOptions();
    return command.run(argc, argv, [&] {
        const std::filesystem::path directory = command.outputDirectory();
        options.read.trusted = command.trusted();
        common::Workspace workspace = command.workspace();
        common::Outputs outputs;
        const Report report = compress(command.input(), directory, command.outputStem(), options, workspace, outputs,
                                       command.diagnostics());
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
} // namespace meshtinker::compress

int main(int argc, char** argv) { return meshtinker::compress::run(argc, argv); }
