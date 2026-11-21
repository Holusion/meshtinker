// meshtinker-weld: merges the vertices of each primitive that are equal in
// every attribute (docs/weld.md).

#include "weld.hpp"

#include "meshtinker/cli/command.hpp"

#include <iostream>

namespace meshtinker::weld {
namespace {

int run(int argc, char** argv) {
    cli::Command command("weld", "Merge identical vertices in a glTF or GLB model",
                         "Merges the vertices of each triangle primitive that are equal in every attribute and "
                         "morph target, bit for bit. Welded primitives are written uncompressed; Draco and meshopt "
                         "primitives, and those that share vertices or can't be read, are copied as they are.",
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
        const Report report = weld(command.input(), directory, command.outputStem(), options, workspace,
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
} // namespace meshtinker::weld

int main(int argc, char** argv) { return meshtinker::weld::run(argc, argv); }
