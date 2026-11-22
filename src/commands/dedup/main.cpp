// meshtinker-dedup: stores identical images, samplers, textures, materials,
// accessors and meshes once (docs/dedup.md).

#include "dedup.hpp"

#include "meshtinker/cli/command.hpp"

#include <iostream>

namespace meshtinker::dedup {
namespace {

int run(int argc, char** argv) {
    cli::Command command("dedup", "Store identical images, materials, meshes and accessors once",
                         "Merges images with the same bytes, then samplers, textures, materials, accessors and "
                         "meshes that are equal but for their name, into the first of each, and removes the "
                         "others. Accessors in meshopt views and sparse accessors are left as they are.",
                         cli::Kind::transform);
    Options options;
    command.addInput();
    command.addOutput();
    command.addJsonOption();
    command.addTrustedOption();
    command.addFlag("--dry-run", options.dry_run, "Count what would merge, and write nothing", true);
    return command.run(argc, argv, [&] {
        options.read.trusted = command.trusted();
        const std::filesystem::path directory = options.dry_run ? std::filesystem::path() : command.outputDirectory();
        const Report report =
            dedup(command.input(), directory, command.outputStem(), options, command.outputs(), command.diagnostics());
        const nlohmann::ordered_json json = toJson(report, command.jsonHeader(), command.diagnostics());
        if (!options.dry_run) {
            command.publish(json);
        }
        if (command.json()) {
            std::cout << json.dump(2) << '\n';
        } else {
            printText(std::cout, report);
        }
        return 0;
    });
}

} // namespace
} // namespace meshtinker::dedup

int main(int argc, char** argv) { return meshtinker::dedup::run(argc, argv); }
