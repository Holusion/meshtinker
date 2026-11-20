// meshtinker-simplify: reduces the triangles of a model's meshes, up to a
// relative error (docs/simplify.md).

#include "simplify.hpp"

#include "meshtinker/cli/command.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>

namespace meshtinker::simplify {
namespace {

int run(int argc, char** argv) {
    cli::Command command("simplify", "Reduce the triangles of a glTF or GLB model",
                         "Collapses edges with meshoptimizer until its estimate of how far the surface moved reaches "
                         "the error, keeping UV seams and the borders between tiles drawn in one space. Simplified "
                         "primitives are written uncompressed; others are copied as they are.");
    Options options;
    command.addInput();
    command.addOutput();
    std::ostringstream default_error_text;
    default_error_text << default_error;
    command.app()
        .add_option("--error", options.error,
                    "How far the surface may move, as a fraction of the size of the meshes drawn with it (default " +
                        default_error_text.str() + ")")
        ->type_name("E")
        ->check([](const std::string& value) {
            char* end = nullptr;
            const double error = std::strtod(value.c_str(), &end);
            const bool valid = end != value.c_str() && *end == '\0' && error > 0 && error <= 1;
            return valid ? std::string() : std::string("E must be a number above 0 and at most 1");
        });
    command.addJsonOption();
    command.addTrustedOption();
    command.addWorkspaceOptions();
    return command.run(argc, argv, [&] {
        const std::filesystem::path directory = command.outputDirectory();
        options.read.trusted = command.trusted();
        common::Workspace workspace = command.workspace();
        common::Outputs outputs;
        const Report report = simplify(command.input(), directory, command.outputStem(), options, workspace, outputs,
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
} // namespace meshtinker::simplify

int main(int argc, char** argv) { return meshtinker::simplify::run(argc, argv); }
