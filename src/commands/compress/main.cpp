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
                         "12-bit texture coordinates. Draco and meshopt primitives are copied as they are.",
                         cli::Kind::transform);
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
        const Report report = compress(command.input(), directory, command.outputStem(), options, workspace,
                                       command.outputs(), command.diagnostics());
        if (chain::StageWriter* stage = command.stage(); stage != nullptr && report.compressed > 0) {
            // The preset's bits (docs/compress.md#the-preset). Positions stay
            // float in meshes that can't take a grid.
            nlohmann::ordered_json round = {{"source", "EXT_meshopt_compression"}, {"primitives", report.compressed}};
            if (report.quantized_meshes > 0) {
                round["positionBits"] = 14;
            }
            round["normalBits"] = 8;
            round["texcoordBits"] = 12;
            stage->lossy().addRound("geometryQuantization", round, &command.diagnostics());
        }
        const nlohmann::ordered_json json = toJson(report, command.jsonHeader(), command.diagnostics());
        command.publish(json);
        if (command.json()) {
            std::cout << json.dump(2) << '\n';
        } else {
            printText(std::cout, report);
        }
        return 0;
    });
}

} // namespace
} // namespace meshtinker::compress

int main(int argc, char** argv) { return meshtinker::compress::run(argc, argv); }
