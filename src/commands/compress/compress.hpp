#pragma once

#include "meshtinker/common/diagnostics.hpp"
#include "meshtinker/common/outputs.hpp"
#include "meshtinker/common/workspace.hpp"
#include "meshtinker/gltf/document.hpp"
#include "meshtinker/gltf/writer.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace meshtinker::compress {

struct Options {
    gltf::ReadOptions read;
};

struct Report {
    std::uint64_t compressed = 0;
    // Primitives copied as they are: Draco, already meshopt-compressed, or unreadable.
    std::uint64_t kept_draco = 0;
    std::uint64_t kept_meshopt = 0;
    std::uint64_t kept_unreadable = 0;
    // Meshes whose positions went on a grid, and those left float.
    std::uint64_t quantized_meshes = 0;
    std::uint64_t float_meshes = 0;
    // Nodes added to carry a mesh's grid, where the mesh's node couldn't.
    std::uint64_t added_nodes = 0;
    // Bytes of the buffer views the compressed primitives read in the input,
    // and of their compressed views in the output.
    std::uint64_t input_bytes = 0;
    std::uint64_t output_bytes = 0;
    std::vector<gltf::WrittenFile> files;
};

// Quantizes and compresses the geometry of `input` with EXT_meshopt_compression
// and writes the model as `<stem>.gltf`, `<stem>.bin` and its images in
// `directory` (docs/compress.md).
Report compress(const std::filesystem::path& input, const std::filesystem::path& directory, const std::string& stem,
                const Options& options, common::Workspace& workspace, common::Outputs& outputs,
                common::Diagnostics& diagnostics);

nlohmann::ordered_json toJson(const Report& report, nlohmann::ordered_json header, const common::Diagnostics& diagnostics);
void printText(std::ostream& out, const Report& report);

} // namespace meshtinker::compress
