// meshtinker-ktx: encodes a model's images as KTX2 with Basis Universal
// (docs/ktx.md).

#include "ktx.hpp"

#include "meshtinker/cli/command.hpp"

#include <cstdlib>
#include <iostream>
#include <map>

namespace meshtinker::ktx {
namespace {

int run(int argc, char** argv) {
    cli::Command command("ktx", "Encode the images of a glTF or GLB model as KTX2",
                         "Encodes PNG, JPEG, WebP and AVIF images with Basis Universal and references them through "
                         "KHR_texture_basisu: ETC1S for color, UASTC for normal maps and other data. The "
                         "textures' other sources are removed.",
                         cli::Kind::transform);
    Options options;
    command.addInput();
    command.addOutput();
    const std::map<std::string, Quality> qualities = {{"Highest", Quality::highest},
                                                      {"High", Quality::high},
                                                      {"Medium", Quality::medium},
                                                      {"Low", Quality::low},
                                                      {"Thumb", Quality::thumb}};
    command.app()
        .add_option("--quality", options.quality, "ETC1S quality, as eCorpus's presets name it (default High)")
        ->type_name("Q")
        ->transform(CLI::CheckedTransformer(qualities))
        ->option_text("<Highest|High|Medium|Low|Thumb>");
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
    command.app()
        .add_option("--min-size", options.min_size,
                    "Leave images whose longest side is N texels or less as they are")
        ->type_name("N");
    command.addJsonOption();
    command.addTrustedOption();
    return command.run(argc, argv, [&] {
        const std::filesystem::path directory = command.outputDirectory();
        options.read.trusted = command.trusted();
        const Report report =
            ktx(command.input(), directory, command.outputStem(), options, command.outputs(), command.diagnostics());
        if (chain::StageWriter* stage = command.stage(); stage != nullptr) {
            for (const EncodedImage& image : report.encoded) {
                nlohmann::ordered_json round = {{"image", image.image},
                                                {"format", "image/ktx2"},
                                                {"codec", codecName(image.codec)}};
                if (image.codec == Codec::etc1s) {
                    round["qualityLevel"] = settings(report.quality).etc1s_quality;
                } else {
                    round["uastcLevel"] = settings(report.quality).uastc_level;
                }
                stage->lossy().addRound("imageEncoding", round, &command.diagnostics());
            }
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
} // namespace meshtinker::ktx

int main(int argc, char** argv) { return meshtinker::ktx::run(argc, argv); }
