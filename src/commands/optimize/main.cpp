// meshtinker-optimize: runs the other commands on a chain, with the steps and
// options that suit the model (docs/optimize.md).

#include "optimize.hpp"

#include "meshtinker/cli/command.hpp"
#include "meshtinker/common/process.hpp"
#include "meshtinker/common/workspace.hpp"

#include <iostream>
#include <map>

namespace meshtinker::optimize {
namespace {

int run(int argc, char** argv) {
    cli::Command command("optimize", "Optimize a glTF or GLB model in one step",
                         "Imports the model into a chain in its workspace, runs the commands that suit it (normals, "
                         "weld, dedup, simplify, uv, ktx or avif, compress) with the options of the preset and "
                         "quality, and exports the result. Each step's warnings appear as it runs.",
                         cli::Kind::files);
    Options options;
    command.addInput();
    command.addOutput(cli::OutputFormats::gltf_or_glb);
    const std::map<std::string, Quality> qualities = {{"Highest", Quality::highest},
                                                      {"High", Quality::high},
                                                      {"Medium", Quality::medium},
                                                      {"Low", Quality::low},
                                                      {"Thumb", Quality::thumb}};
    command.app()
        .add_option("--quality", options.settings.quality,
                    "Geometry and texture quality, as eCorpus's presets name it (default High)")
        ->transform(CLI::CheckedTransformer(qualities))
        ->option_text("<Highest|High|Medium|Low|Thumb>");
    const std::map<std::string, Preset> presets = {{"slow", Preset::slow}, {"fast", Preset::fast}};
    command.app()
        .add_option("--preset", options.settings.preset,
                    "slow for the asset to publish, fast for a temporary model: no uv, AVIF at a low effort "
                    "(default slow)")
        ->transform(CLI::CheckedTransformer(presets))
        ->option_text("<slow|fast>");
    const std::map<std::string, Textures> textures = {{"ktx", Textures::ktx}, {"avif", Textures::avif}};
    command.app()
        .add_option("--textures", options.settings.textures,
                    "KTX2, which stays compressed on the GPU, or AVIF, smaller to download (default ktx)")
        ->transform(CLI::CheckedTransformer(textures))
        ->option_text("<ktx|avif>");
    command.addJsonOption();
    command.addTrustedOption();
    command.addWorkspaceOptions();
    return command.run(argc, argv, [&] {
        options.trusted = command.trusted();
        options.own_directory = common::currentExecutable(argv[0]).parent_path();
        command.outputDirectory();
        common::Workspace workspace = command.workspace();
        const Report report = optimize(command.input(), *command.outputArgument(), workspace.directory(), options);
        const nlohmann::ordered_json json = toJson(report, command.jsonHeader(), command.diagnostics());
        if (command.json()) {
            std::cout << json.dump(2) << '\n';
        } else {
            printText(std::cout, report);
        }
        return 0;
    });
}

} // namespace
} // namespace meshtinker::optimize

int main(int argc, char** argv) { return meshtinker::optimize::run(argc, argv); }
