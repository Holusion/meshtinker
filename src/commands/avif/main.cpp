// meshtinker-avif: encodes a model's images as AVIF with libavif and libaom
// (docs/avif.md).

#include "avif.hpp"

#include "meshtinker/cli/command.hpp"

#include <cstdlib>
#include <iostream>
#include <map>

namespace meshtinker::avif {
namespace {

int run(int argc, char** argv) {
    cli::Command command("avif", "Encode the images of a glTF or GLB model as AVIF",
                         "Encodes PNG and JPEG images with libavif and libaom and references them through "
                         "EXT_texture_avif, which becomes required: color in 4:2:0, normal maps and other data in "
                         "4:4:4. The textures' other sources are removed; KTX2 textures are kept.",
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
        .add_option("--quality", options.quality,
                    "Quality, as eCorpus's presets name it: libavif quality 80, 65, 50, 40, 30 (default High)")
        ->type_name("Q")
        ->transform(CLI::CheckedTransformer(qualities))
        ->option_text("<Highest|High|Medium|Low|Thumb>");
    command.app()
        .add_option("--effort", options.effort,
                    "Encoder effort, 0 (fastest) to 9, as sharp counts it; 0 and 1 inflate some images (default 3)")
        ->type_name("E")
        ->check(CLI::Range(0, 9));
    command.app()
        .add_option("--max-size", options.max_size,
                    "Scale down images whose longest side is over N texels, keeping their aspect ratio")
        ->type_name("N")
        ->check([](const std::string& value) {
            char* end = nullptr;
            const unsigned long size = std::strtoul(value.c_str(), &end, 10);
            const bool valid = end != value.c_str() && *end == '\0' && size >= 1 && size <= (1ul << 24);
            return valid ? std::string() : std::string("N must be a whole number from 1 to 16777216");
        });
    command.addJsonOption();
    command.addTrustedOption();
    return command.run(argc, argv, [&] {
        const std::filesystem::path directory = command.outputDirectory();
        options.read.trusted = command.trusted();
        const Report report =
            avif(command.input(), directory, command.outputStem(), options, command.outputs(), command.diagnostics());
        if (chain::StageWriter* stage = command.stage(); stage != nullptr) {
            for (const EncodedImage& entry : report.encoded) {
                stage->lossy().addRound("imageEncoding",
                                        {{"image", entry.image.image},
                                         {"format", "image/avif"},
                                         {"quality", qualityLevel(report.quality)},
                                         {"effort", report.effort},
                                         {"subsampling", subsamplingName(entry.subsampling)}},
                                        &command.diagnostics());
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
} // namespace meshtinker::avif

int main(int argc, char** argv) { return meshtinker::avif::run(argc, argv); }
