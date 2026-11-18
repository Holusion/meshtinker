// meshtinker-placeholder: builds a low-poly stand-in for a model, to display
// while the model is processed (docs/placeholder.md).

#include "placeholder.hpp"

#include "meshtinker/cli/command.hpp"
#include "meshtinker/common/failure.hpp"

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
    fs::path input;
    fs::path output;
    Options options;
    command.app().add_option("input", input, "The .gltf or .glb file")->required();
    command.app()
        .add_option("output", output, "The .gltf file to write. Its directory must exist; no file is overwritten")
        ->required()
        ->check([](const std::string& value) {
            return fs::u8path(value).extension() == ".gltf" ? std::string() : std::string("OUTPUT must end in .gltf");
        });
    command.app()
        .add_option("--resolution", options.resolution,
                    "Cells along the model's longest axis, " + std::to_string(min_resolution) + " to " +
                        std::to_string(max_resolution) + " (default " + std::to_string(default_resolution) + ")")
        ->type_name("N")
        ->check(CLI::Range(min_resolution, max_resolution).description(""));
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
        options.read.trusted = command.trusted();
        common::Workspace workspace(command.tmpDir().value_or(directory), command.keepTmp());
        common::Outputs outputs;
        const Report report =
            build(input, directory, output.stem().u8string(), options, workspace, outputs, command.diagnostics());
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
