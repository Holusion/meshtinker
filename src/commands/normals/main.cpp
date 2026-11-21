// meshtinker-normals: writes smooth normals where a model has none, or only
// flat ones (docs/normals.md).

#include "normals.hpp"

#include "meshtinker/cli/command.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>

namespace meshtinker::normals {
namespace {

int run(int argc, char** argv) {
    cli::Command command("normals", "Write smooth normals where a glTF or GLB model has none or flat ones",
                         "Computes smooth normals for the triangle primitives that have no NORMAL, or one normal per "
                         "face, summed over positions and across the primitives that share them, with edges sharper "
                         "than the crease angle kept hard. Authored normals are kept. Rewritten primitives are "
                         "written uncompressed.",
                         cli::Kind::transform);
    Options options;
    command.addInput();
    command.addOutput();
    std::ostringstream default_angle;
    default_angle << default_crease_angle;
    command.app()
        .add_option("--crease-angle", options.crease_angle,
                    "Edges whose faces meet at more than this angle, in degrees, stay hard: 0 keeps every edge hard, "
                    "180 none (default " +
                        default_angle.str() + ")")
        ->type_name("DEGREES")
        ->check([](const std::string& value) {
            char* end = nullptr;
            const double angle = std::strtod(value.c_str(), &end);
            const bool valid = end != value.c_str() && *end == '\0' && angle >= 0 && angle <= 180;
            return valid ? std::string() : std::string("DEGREES must be a number from 0 to 180");
        });
    command.addFlag("--keep-flat", options.keep_flat,
                    "Keep flat normals (one per face) as they are: only primitives without normals get them");
    command.addJsonOption();
    command.addTrustedOption();
    command.addWorkspaceOptions();
    return command.run(argc, argv, [&] {
        const std::filesystem::path directory = command.outputDirectory();
        options.read.trusted = command.trusted();
        common::Workspace workspace = command.workspace();
        const Report report = normals(command.input(), directory, command.outputStem(), options, workspace,
                                      command.outputs(), command.diagnostics());
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
} // namespace meshtinker::normals

int main(int argc, char** argv) { return meshtinker::normals::run(argc, argv); }
