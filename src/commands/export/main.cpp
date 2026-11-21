// meshtinker-export: writes the last stage of a chain as one .gltf or .glb
// file (docs/chaining.md).

#include "meshtinker/cli/command.hpp"
#include "meshtinker/gltf/layout.hpp"
#include "meshtinker/gltf/selection.hpp"
#include "meshtinker/gltf/writer.hpp"

#include "meshtinker/common/failure.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>

// `export` is a C++ keyword, hence the namespace name.
namespace meshtinker::exporter {
namespace {

namespace fs = std::filesystem;

const char* roleName(gltf::FileRole role) {
    switch (role) {
    case gltf::FileRole::gltf:
        return "gltf";
    case gltf::FileRole::glb:
        return "glb";
    case gltf::FileRole::buffer:
        return "buffer";
    case gltf::FileRole::image:
        return "image";
    }
    return "";
}

// Whether `path` is `directory` or lies inside it, symlinks resolved.
bool isWithin(const fs::path& path, const fs::path& directory) {
    const auto resolved = [](const fs::path& p) {
        fs::path canonical = fs::weakly_canonical(p);
        // `work/` ends in an empty element, which `work/out` doesn't have.
        return canonical.filename().empty() ? canonical.parent_path() : canonical;
    };
    const fs::path inner = resolved(path);
    const fs::path outer = resolved(directory);
    return std::mismatch(outer.begin(), outer.end(), inner.begin(), inner.end()).first == outer.end();
}

int run(int argc, char** argv) {
    cli::Command command("export", "Write the last stage of a chain as a .gltf or .glb file",
                         "A .gltf output gets its .bin and images beside it, as in the stage. A .glb output holds "
                         "everything: the images go into its binary chunk. The model is written as the stage holds "
                         "it.",
                         cli::Kind::terminal);
    command.addInput();
    command.addOutput();
    bool remove_chain = false;
    command.addFlag("--remove-chain", remove_chain,
                    "Remove the chain's directory, with everything in it, once the output is written");
    command.addJsonOption();
    return command.run(argc, argv, [&] {
        const fs::path directory = command.outputDirectory();
        const chain::Chain& chain = *command.chain();
        if (remove_chain && isWithin(directory, chain.directory())) {
            throw common::usageError("usage", "--remove-chain removes the chain's directory: write OUTPUT outside it");
        }
        // Removing the chain needs its lock, and the last stage read under it.
        std::optional<common::FileLock> lock;
        if (remove_chain) {
            lock.emplace(chain.lock());
        }
        const chain::StageName stage = chain.last();
        const fs::path input = chain.model(stage);
        if (remove_chain) {
            // The chain goes, so its files can become the output's.
            command.outputs().linkFrom(input.parent_path());
        }
        const gltf::Document document = gltf::readNormalized(input, {}, command.diagnostics());
        const gltf::Layout layout =
            gltf::normalizedLayout(document, gltf::selectScene(document), command.diagnostics());
        const fs::path& output = *command.outputArgument();
        const std::vector<gltf::WrittenFile> files =
            output.extension() == ".glb"
                ? gltf::writeGlb(document, layout, output, command.outputs())
                : gltf::writeSeparate(document, layout, directory, command.outputStem(), command.outputs());
        command.publish({});
        if (lock) {
            chain.remove(*lock, command.diagnostics());
        }
        if (command.json()) {
            nlohmann::ordered_json report = command.jsonHeader();
            report["stage"] = stage.text();
            nlohmann::ordered_json written = nlohmann::ordered_json::array();
            for (const gltf::WrittenFile& file : files) {
                written.push_back({{"role", roleName(file.role)}, {"name", file.name}, {"byteSize", file.size}});
            }
            report["files"] = std::move(written);
            report["removedChain"] = remove_chain;
            report["diagnostics"] = command.diagnostics().toJson();
            std::cout << report.dump(2) << '\n';
        } else {
            std::cout << "Stage: " << stage.text() << '\n';
            for (const gltf::WrittenFile& file : files) {
                std::cout << "  " << file.name << "  " << roleName(file.role) << ", " << file.size << " bytes\n";
            }
        }
        return 0;
    });
}

} // namespace
} // namespace meshtinker::exporter

int main(int argc, char** argv) { return meshtinker::exporter::run(argc, argv); }
