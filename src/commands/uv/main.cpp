// meshtinker-uv: repacks a model's UV charts and bakes its textures into the
// new layout (docs/uv.md).

#include "uv.hpp"

#include "meshtinker/cli/command.hpp"

#include <cstdlib>
#include <iostream>

namespace meshtinker::uv {
namespace {

int run(int argc, char** argv) {
    cli::Command command("uv", "Pack the UV charts of a glTF or GLB model tighter, and bake its textures",
                         "Moves the UV charts of each material closer together, by whole texels, and copies their "
                         "texels into smaller images, written as PNG. Charts are not rotated, scaled or re-cut. "
                         "Materials whose images wouldn't shrink by 10 % are left as they are.",
                         cli::Kind::transform);
    Options options;
    command.addInput();
    command.addOutput();
    command.app()
        .add_option("--max-size", options.max_size,
                    "Scale down images whose longest side is over N texels, keeping their aspect ratio")
        ->type_name("N")
        ->check([](const std::string& value) {
            char* end = nullptr;
            const unsigned long size = std::strtoul(value.c_str(), &end, 10);
            const bool valid = end != value.c_str() && *end == '\0' && size >= 4 && size <= (1ul << 24);
            return valid ? std::string() : std::string("N must be a whole number from 4 to 16777216");
        });
    // Packing settings, for measurements (docs/uv.md#time-and-memory).
    command.addHiddenOption("--padding", options.padding);
    command.addFlag("--brute-force", options.brute_force, "xatlas's brute-force placement", true);
    command.addHiddenOption("--min-gain", options.min_gain);
    command.addJsonOption();
    command.addTrustedOption();
    command.addWorkspaceOptions();
    return command.run(argc, argv, [&] {
        const std::filesystem::path directory = command.outputDirectory();
        options.read.trusted = command.trusted();
        common::Workspace workspace = command.workspace();
        const Report report = uv(command.input(), directory, command.outputStem(), options, workspace,
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
} // namespace meshtinker::uv

int main(int argc, char** argv) { return meshtinker::uv::run(argc, argv); }
