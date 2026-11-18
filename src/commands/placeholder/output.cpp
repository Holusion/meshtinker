#include "output.hpp"

#include "meshtinker/cli/command.hpp"
#include "meshtinker/gltf/uri.hpp"

#include <cstring>
#include <limits>

namespace meshtinker::placeholder {
namespace {

using gltf::Json;

// glTF reserves each index type's maximum value for primitive restart.
constexpr std::size_t short_vertices = std::numeric_limits<std::uint16_t>::max();
constexpr std::uint64_t vertex_stride = 24;

std::uint64_t align4(std::uint64_t n) { return (n + 3) & ~std::uint64_t{3}; }

template <typename T>
common::ByteView viewOf(const std::vector<T>& values) {
    return {reinterpret_cast<const std::uint8_t*>(values.data()), values.size() * sizeof(T)};
}

} // namespace

std::vector<gltf::WrittenFile> writePlaceholder(const Mesh& mesh, const Model& model,
                                                const std::filesystem::path& directory, const std::string& stem,
                                                common::Outputs& outputs) {
    const bool short_indices = mesh.positions.size() <= short_vertices;
    const std::uint64_t index_bytes = mesh.indices.size() * (short_indices ? 2U : 4U);
    const std::uint64_t vertex_offset = align4(index_bytes);
    const std::uint64_t vertex_bytes = mesh.positions.size() * vertex_stride;
    std::array<float, 3> min{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
                             std::numeric_limits<float>::infinity()};
    std::array<float, 3> max{-min[0], -min[1], -min[2]};
    for (const auto& p : mesh.positions) {
        for (std::size_t k = 0; k < 3; ++k) {
            min[k] = std::min(min[k], p[k]);
            max[k] = std::max(max[k], p[k]);
        }
    }

    // Positions stay in the frame of the source's first mesh node, so the
    // placeholder carries the transforms leading there to overlay the model.
    Json nodes = Json::array();
    for (const Json& transform : model.frame()) {
        nodes.push_back(transform);
    }
    if (nodes.empty()) {
        nodes.push_back(Json::object());
    }
    for (std::size_t i = 0; i + 1 < nodes.size(); ++i) {
        nodes[i]["children"] = Json::array({i + 1});
    }
    nodes.back()["name"] = "placeholder";
    nodes.back()["mesh"] = 0;

    const std::string bin_name = stem + ".bin";
    // In the profile's buffer order: the indices, then the vertices.
    Json json = Json::object();
    json["asset"] = {{"version", "2.0"}, {"generator", std::string("meshtinker ") + cli::version()}};
    json["scene"] = 0;
    json["scenes"] = Json::array({{{"nodes", Json::array({0})}}});
    json["nodes"] = nodes;
    json["meshes"] = Json::array(
        {{{"name", "placeholder"},
          {"primitives", Json::array({{{"attributes", {{"POSITION", 0}, {"NORMAL", 1}}}, {"indices", 2}}})}}});
    json["accessors"] = Json::array(
        {{{"bufferView", 1}, {"componentType", 5126}, {"count", mesh.positions.size()}, {"type", "VEC3"},
          {"min", min}, {"max", max}},
         {{"bufferView", 1}, {"byteOffset", 12}, {"componentType", 5126}, {"count", mesh.positions.size()},
          {"type", "VEC3"}},
         {{"bufferView", 0},
          {"componentType", short_indices ? 5123 : 5125},
          {"count", mesh.indices.size()},
          {"type", "SCALAR"}}});
    json["bufferViews"] = Json::array(
        {{{"buffer", 0}, {"byteLength", index_bytes}, {"target", 34963}},
         {{"buffer", 0}, {"byteOffset", vertex_offset}, {"byteLength", vertex_bytes}, {"byteStride", vertex_stride},
          {"target", 34962}}});
    json["buffers"] =
        Json::array({{{"uri", gltf::encodeUriSegment(bin_name)}, {"byteLength", vertex_offset + vertex_bytes}}});

    const std::string gltf_name = stem + ".gltf";
    common::OutputFile gltf_file = outputs.create(directory / std::filesystem::u8path(gltf_name));
    gltf_file.write(json.dump());
    const std::uint64_t gltf_size = gltf_file.position();
    gltf_file.close();

    common::OutputFile bin = outputs.create(directory / std::filesystem::u8path(bin_name));
    if (short_indices) {
        std::vector<std::uint16_t> narrow(mesh.indices.begin(), mesh.indices.end());
        bin.write(viewOf(narrow));
    } else {
        bin.write(viewOf(mesh.indices));
    }
    bin.align(4);
    std::vector<float> vertices;
    vertices.reserve(mesh.positions.size() * 6);
    for (std::size_t v = 0; v < mesh.positions.size(); ++v) {
        vertices.insert(vertices.end(), mesh.positions[v].begin(), mesh.positions[v].end());
        vertices.insert(vertices.end(), mesh.normals[v].begin(), mesh.normals[v].end());
    }
    bin.write(viewOf(vertices));
    const std::uint64_t bin_size = bin.position();
    bin.close();
    return {{gltf::FileRole::gltf, gltf_name, gltf_size}, {gltf::FileRole::buffer, bin_name, bin_size}};
}

nlohmann::ordered_json toJson(const Report& report, nlohmann::ordered_json header,
                              const common::Diagnostics& diagnostics) {
    Json json = std::move(header);
    json["input"] = {{"faces", report.source_faces}};
    json["grid"] = {{"resolution", report.resolution},
                    {"cells", report.grid},
                    {"cellSize", report.cell_size},
                    {"passes", report.passes},
                    {"droppedParts", report.dropped_parts}};
    Json files = Json::array();
    for (const gltf::WrittenFile& file : report.files) {
        files.push_back({{"name", file.name}, {"byteSize", file.size}});
    }
    json["output"] = {{"vertices", report.vertices}, {"faces", report.faces}, {"files", files}};
    json["diagnostics"] = diagnostics.toJson();
    return json;
}

void printText(std::ostream& out, const Report& report) {
    out << "Placeholder: " << report.faces << " faces, " << report.vertices << " vertices, from "
        << report.source_faces << " faces\n";
    out << "Grid: " << report.grid[0] << " x " << report.grid[1] << " x " << report.grid[2] << " cells of "
        << report.cell_size << " (resolution " << report.resolution << ", " << report.passes
        << (report.passes == 1 ? " pass" : " passes") << ")";
    if (report.dropped_parts > 0) {
        out << ", " << report.dropped_parts << " small parts left out";
    }
    out << '\n';
    for (const gltf::WrittenFile& file : report.files) {
        out << "  " << file.name << "  " << file.size << " bytes\n";
    }
}

} // namespace meshtinker::placeholder
