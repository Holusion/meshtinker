#include "meshtinker/analysis/report.hpp"

#include <cstdio>

namespace meshtinker::analysis {
namespace {

using Json = nlohmann::ordered_json;

template <typename T>
Json optional(const std::optional<T>& value) {
    return value ? Json(*value) : Json(nullptr);
}

std::string bytes(std::uint64_t size) {
    char text[32];
    if (size < 1000) {
        std::snprintf(text, sizeof text, "%llu B", static_cast<unsigned long long>(size));
    } else if (size < 1000ull * 1000) {
        std::snprintf(text, sizeof text, "%.1f kB", static_cast<double>(size) / 1e3);
    } else if (size < 1000ull * 1000 * 1000) {
        std::snprintf(text, sizeof text, "%.1f MB", static_cast<double>(size) / 1e6);
    } else {
        std::snprintf(text, sizeof text, "%.2f GB", static_cast<double>(size) / 1e9);
    }
    return text;
}

std::string percent(const std::optional<double>& fraction) {
    if (!fraction) {
        return "not computed";
    }
    char text[32];
    std::snprintf(text, sizeof text, "%.1f %%", *fraction * 100.0);
    return text;
}

std::string plural(std::uint64_t count, const char* singular, const char* plural_form) {
    return std::to_string(count) + " " + (count == 1 ? singular : plural_form);
}

std::string vector3(const std::array<double, 3>& v) {
    char text[96];
    std::snprintf(text, sizeof text, "[%g, %g, %g]", v[0], v[1], v[2]);
    return text;
}

} // namespace

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

std::uint64_t Report::byteSize() const {
    std::uint64_t total = 0;
    for (const File& file : files) {
        total += file.size;
    }
    return total;
}

nlohmann::ordered_json toJson(const Report& report, Json header, const common::Diagnostics& diagnostics) {
    // Without data, the fields that need the referenced files are left out,
    // not null: null means a figure couldn't be determined.
    const bool data = report.data;
    Json json = std::move(header);
    json["data"] = data;
    Json files = Json::array();
    for (const Report::File& file : report.files) {
        files.push_back({{"role", roleName(file.role)}, {"name", file.name}, {"byteSize", file.size}});
    }
    json["input"] = Json::object();
    if (data) {
        json["input"]["byteSize"] = report.byteSize();
    }
    json["input"]["files"] = files;
    // mesh2gltf's shape: each entry lists the paths a file is looked for at,
    // in order. meshtinker looks at one.
    const auto paths = [](const std::vector<std::string>& list) {
        Json entries = Json::array();
        for (const std::string& path : list) {
            entries.push_back(Json::array({path}));
        }
        return entries;
    };
    json["requires"] = paths(report.required_files);
    json["extensions"] = {{"used", report.extensions_used}, {"required", report.extensions_required}};
    if (report.scene) {
        const Report::Scene& scene = *report.scene;
        Json bounds = nullptr;
        if (scene.bounds) {
            bounds = {{"min", scene.bounds->min}, {"max", scene.bounds->max}};
        }
        json["scene"] = {{"index", scene.index},
                         {"name", optional(scene.name)},
                         {"firstNamedRootNode", optional(scene.first_named_root_node)}};
        if (data) {
            json["scene"]["bounds"] = bounds;
        }
    } else {
        json["scene"] = nullptr;
    }
    const Report::Geometry& g = report.geometry;
    json["geometry"] = {{"meshes", g.meshes},
                        {"primitives", g.primitives},
                        {"vertices", g.vertices},
                        {"faces", g.faces},
                        {"primitivesByEncoding",
                         {{"plain", g.plain}, {"meshopt", g.meshopt}, {"draco", g.draco}, {"quantized", g.quantized}}}};
    Json images = Json::array();
    for (const Report::Image& image : report.images) {
        Json uses = Json::array();
        for (const Report::Use& use : image.uses) {
            uses.push_back({{"material", use.material}, {"slot", use.slot}, {"texture", use.texture}});
        }
        Json entry = {{"index", image.index}, {"name", optional(image.name)}, {"file", optional(image.file)}};
        if (data) {
            entry["mimeType"] = optional(image.mime_type);
            entry["width"] = optional(image.width);
            entry["height"] = optional(image.height);
            entry["byteSize"] = image.byte_size;
        }
        entry["uses"] = uses;
        if (data) {
            entry["occupancy"] = optional(image.occupancy);
        }
        images.push_back(std::move(entry));
    }
    json["images"] = images;
    if (data) {
        json["texels"] = {{"total", report.texels_total}, {"occupancy", optional(report.texels_occupancy)}};
    }
    json["diagnostics"] = diagnostics.toJson();
    return json;
}

void printText(std::ostream& out, const Report& report) {
    const bool data = report.data;
    if (data) {
        out << "Files: " << bytes(report.byteSize()) << " in " << plural(report.files.size(), "file", "files") << '\n';
    } else {
        out << "Files: not read, besides the model file\n";
    }
    for (const Report::File& file : report.files) {
        out << "  " << file.name << "  " << roleName(file.role) << ", " << bytes(file.size) << '\n';
    }
    if (!report.required_files.empty()) {
        out << "Requires: ";
        for (std::size_t i = 0; i < report.required_files.size(); ++i) {
            out << (i == 0 ? "" : ", ") << report.required_files[i];
        }
        out << '\n';
    }
    if (report.scene) {
        const Report::Scene& scene = *report.scene;
        out << "Scene: " << scene.index;
        if (scene.name) {
            out << " \"" << *scene.name << "\"";
        }
        if (scene.first_named_root_node) {
            out << ", first named node \"" << *scene.first_named_root_node << "\"";
        }
        out << '\n';
        out << "Bounds: ";
        if (!data) {
            out << "not read\n";
        } else if (scene.bounds) {
            out << vector3(scene.bounds->min) << " to " << vector3(scene.bounds->max) << '\n';
        } else {
            out << "not computed\n";
        }
    } else {
        out << "Scene: none\n";
    }
    const Report::Geometry& g = report.geometry;
    out << "Geometry: " << plural(g.meshes, "mesh", "meshes") << ", " << plural(g.primitives, "primitive", "primitives")
        << ", " << plural(g.vertices, "vertex", "vertices") << ", " << plural(g.faces, "face", "faces") << '\n';
    if (g.meshopt + g.draco + g.quantized > 0) {
        out << "  primitives: " << g.plain << " plain, " << g.meshopt << " meshopt, " << g.draco << " Draco, "
            << g.quantized << " quantized\n";
    }
    out << "Images: " << report.images.size();
    if (data) {
        out << ", " << report.texels_total << " texels in use, occupancy " << percent(report.texels_occupancy);
    }
    out << '\n';
    for (const Report::Image& image : report.images) {
        out << "  " << image.index << " " << (image.file ? *image.file : image.name.value_or("(embedded)"));
        if (data) {
            out << "  " << image.mime_type.value_or("unknown format");
            if (image.width && image.height) {
                out << " " << *image.width << "x" << *image.height;
            }
            out << ", " << bytes(image.byte_size) << ", occupancy " << percent(image.occupancy);
        }
        for (std::size_t i = 0; i < image.uses.size(); ++i) {
            out << (i == 0 ? ", used as " : ", ") << image.uses[i].slot;
        }
        if (image.uses.empty()) {
            out << ", unused";
        }
        out << '\n';
    }
    if (!report.extensions_used.empty()) {
        out << "Extensions:";
        for (const std::string& name : report.extensions_used) {
            out << ' ' << name;
        }
        out << '\n';
    }
}

} // namespace meshtinker::analysis
