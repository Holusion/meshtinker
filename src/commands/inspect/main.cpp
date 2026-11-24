// meshtinker-inspect: statistics about a glTF or GLB file (docs/inspect-report.md).

#include "meshtinker/analysis/pipeline.hpp"
#include "meshtinker/cli/command.hpp"
#include "meshtinker/common/failure.hpp"
#include "meshtinker/image/image.hpp"

#include <filesystem>
#include <iostream>
#include <string>

namespace meshtinker::inspect {
namespace {

int run(int argc, char** argv) {
    cli::Command command("inspect", "Report statistics about a glTF or GLB file",
                         "Sizes, geometry, bounds, images and texture occupancy of the scene the file shows. "
                         "Only the default scene counts, as `import` keeps only "
                         "that one. Given a chain, reads its last stage.",
                         cli::Kind::read_only);
    command.addInput();
    command.addJsonOption();
    command.addTrustedOption();
    command.addWorkspaceOptions();
    // For experiments (docs/inspect-report.md#occupancy-maps): one gray PNG per
    // image, white where the scene samples it.
    std::string maps_directory;
    command.app().add_option("--occupancy-maps", maps_directory)->group("");
    return command.run(argc, argv, [&] {
        common::Workspace workspace = command.workspace();
        analysis::CoverageSink maps;
        if (!maps_directory.empty()) {
            const std::filesystem::path directory = std::filesystem::u8path(maps_directory);
            if (!std::filesystem::is_directory(directory)) {
                throw common::Failure(common::ExitCode::machine, "output-directory-missing",
                                      "the --occupancy-maps directory doesn't exist");
            }
            maps = [&command, directory](const std::vector<std::size_t>& images, const analysis::Coverage& map) {
                image::Image mask;
                mask.width = map.width();
                mask.height = map.height();
                mask.pixels.assign(mask.texels() * 4, 0);
                for (std::uint32_t row = 0; row < mask.height; ++row) {
                    for (std::uint32_t column = 0; column < mask.width; ++column) {
                        if (map.test(column, row)) {
                            mask.pixels[(std::size_t{row} * mask.width + column) * 4] = 255;
                        }
                    }
                }
                const std::vector<std::uint8_t> png = image::encodePng(mask, 1);
                for (const std::size_t index : images) {
                    common::OutputFile file =
                        command.outputs().create(directory / ("occupancy-" + std::to_string(index) + ".png"));
                    file.write({png.data(), png.size()});
                    file.close();
                }
            };
        }
        const analysis::Inspection inspection =
            analysis::inspect(command.input(), {command.trusted()}, workspace, command.diagnostics(), maps);
        // The maps, if any, are published once the report is ready.
        command.publish({});
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
