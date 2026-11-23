#pragma once

#include "meshtinker/common/diagnostics.hpp"
#include "meshtinker/gltf/document.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace meshtinker::analysis {

// The statistics `inspect` and `import` report. docs/inspect-report.md is the
// reference for every field; this is its C++ shape. Indices are those of the
// inspected file.
struct Report {
    struct File {
        gltf::FileRole role;
        std::string name;
        std::uint64_t size = 0;
    };
    struct Bounds {
        std::array<double, 3> min;
        std::array<double, 3> max;
    };
    struct Scene {
        std::size_t index = 0;
        std::optional<std::string> name;
        std::optional<std::string> first_named_root_node;
        // Empty when there's no geometry or it can't be read exactly.
        std::optional<Bounds> bounds;
    };
    struct Geometry {
        std::uint64_t meshes = 0;
        std::uint64_t primitives = 0;
        std::uint64_t vertices = 0;
        std::uint64_t faces = 0;
        std::uint64_t plain = 0;
        std::uint64_t meshopt = 0;
        std::uint64_t draco = 0;
        std::uint64_t quantized = 0;
    };
    struct Use {
        std::size_t material = 0;
        std::string slot;
        std::size_t texture = 0;
    };
    struct Image {
        std::size_t index = 0;
        std::optional<std::string> name;
        // The file name, empty when the image is embedded.
        std::optional<std::string> file;
        std::optional<std::string> mime_type;
        std::optional<std::uint32_t> width;
        std::optional<std::uint32_t> height;
        std::uint64_t byte_size = 0;
        // Where `file` points, relative to the model's folder; not reported.
        std::string path;
        std::vector<Use> uses;
        std::optional<double> occupancy;
        // Texels used, behind occupancy; not reported.
        std::uint64_t used_texels = 0;
    };

    // Whether buffers and images were read: false for `inspect --no-data`,
    // whose report leaves out the fields that need them.
    bool data = true;
    std::vector<File> files;
    // The files the model refers to, as paths relative to its folder: those it
    // is rejected without (buffers, base color images), then the others.
    std::vector<std::string> required_files;
    std::vector<std::string> optional_files;
    std::vector<std::string> extensions_used;
    std::vector<std::string> extensions_required;
    std::optional<Scene> scene;
    Geometry geometry;
    std::vector<Image> images;
    std::uint64_t texels_total = 0;
    std::optional<double> texels_occupancy;

    std::uint64_t byteSize() const;
};

// The report as JSON, after `header` (format and tool), with `diagnostics` last.
nlohmann::ordered_json toJson(const Report& report, nlohmann::ordered_json header, const common::Diagnostics& diagnostics);

// The report as text, for people.
void printText(std::ostream& out, const Report& report);

const char* roleName(gltf::FileRole role);

} // namespace meshtinker::analysis
