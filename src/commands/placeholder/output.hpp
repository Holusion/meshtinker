#pragma once

#include "model.hpp"
#include "surface.hpp"
#include "voxel.hpp"

#include "meshtinker/common/diagnostics.hpp"
#include "meshtinker/common/outputs.hpp"
#include "meshtinker/gltf/writer.hpp"

#include <filesystem>
#include <ostream>
#include <string>
#include <vector>

namespace meshtinker::placeholder {

// Writes `<stem>.gltf` and `<stem>.bin` in `directory`: one mesh in the
// frame's coordinates, under the frame's nodes, in the normalized profile.
// Returns the files written, the .gltf first.
std::vector<gltf::WrittenFile> writePlaceholder(const Mesh& mesh, const Model& model,
                                                const std::filesystem::path& directory, const std::string& stem,
                                                common::Outputs& outputs);

struct Report {
    unsigned resolution = 0;
    std::array<std::size_t, 3> grid{};
    double cell_size = 0;
    unsigned passes = 0;
    std::uint64_t source_faces = 0;
    std::uint64_t dropped_parts = 0;
    std::uint64_t vertices = 0;
    std::uint64_t faces = 0;
    std::vector<gltf::WrittenFile> files;
};

nlohmann::ordered_json toJson(const Report& report, nlohmann::ordered_json header,
                              const common::Diagnostics& diagnostics);
void printText(std::ostream& out, const Report& report);

} // namespace meshtinker::placeholder
