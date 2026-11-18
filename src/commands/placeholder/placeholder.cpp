#include "placeholder.hpp"

#include "meshtinker/gltf/geometry.hpp"
#include "meshtinker/gltf/layout.hpp"

#include <thread>

namespace meshtinker::placeholder {

Report build(const std::filesystem::path& input, const std::filesystem::path& directory, const std::string& stem,
             const Options& options, common::Workspace& workspace, common::Outputs& outputs,
             common::Diagnostics& diagnostics) {
    gltf::Document document = gltf::readNormalized(input, options.read, diagnostics);
    gltf::GeometryReader reader(document, workspace);
    const Model model = Model::read(document, reader, diagnostics);
    const unsigned threads = options.threads > 0 ? options.threads : std::max(1U, std::thread::hardware_concurrency());
    const Voxelization voxels = voxelize(model, options.resolution, threads);
    if (voxels.invalid_faces > 0) {
        diagnostics.warn("index-out-of-range", "Faces with an index out of range were left out of the placeholder",
                         std::to_string(voxels.invalid_faces) + " faces");
    }
    if (voxels.dropped_parts > 0) {
        diagnostics.warn("small-parts-left-out",
                         "Small disconnected parts, such as scan noise, were left out of the placeholder",
                         std::to_string(voxels.dropped_parts) + " parts");
    }
    const Mesh mesh = extractSurface(voxels.grid);

    Report report;
    report.resolution = options.resolution;
    const Lattice& lattice = voxels.grid.lattice;
    report.grid = {lattice.dims[0] - 2, lattice.dims[1] - 2, lattice.dims[2] - 2};
    report.cell_size = lattice.cell;
    report.passes = voxels.passes;
    report.source_faces = model.faces();
    report.dropped_parts = voxels.dropped_parts;
    report.vertices = mesh.positions.size();
    report.faces = mesh.indices.size() / 3;
    report.files = writePlaceholder(mesh, model, directory, stem, outputs);
    return report;
}

} // namespace meshtinker::placeholder
