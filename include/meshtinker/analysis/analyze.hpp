#pragma once

#include "meshtinker/analysis/coverage.hpp"
#include "meshtinker/analysis/report.hpp"
#include "meshtinker/common/diagnostics.hpp"
#include "meshtinker/common/workspace.hpp"
#include "meshtinker/gltf/document.hpp"
#include "meshtinker/gltf/layout.hpp"
#include "meshtinker/gltf/selection.hpp"
#include "meshtinker/gltf/writer.hpp"

#include <functional>
#include <vector>

namespace meshtinker::analysis {

// Receives a texture coverage map and the images it is the map of.
using CoverageSink = std::function<void(const std::vector<std::size_t>& images, const Coverage& map)>;

// Stage 1, document fields: what the JSON, file sizes and image headers say.
// Everything except bounds and occupancy (docs/inspect-report.md, "Cost").
// A fast analysis can stop here.
Report analyzeDocument(const gltf::Document& document, const gltf::Selection& selection,
                       common::Diagnostics& diagnostics);

// Stage 2, data fields: reads vertex data for the scene's bounds, and
// rasterizes UV triangles for texture occupancy. Large working sets go in
// `workspace`.
// `maps`, when set, is given each coverage map once its triangles are drawn,
// with the indices of the images that share it (same size, same draws).
void analyzeData(gltf::Document& document, const gltf::Selection& selection, Report& report,
                 common::Workspace& workspace, common::Diagnostics& diagnostics, const CoverageSink& maps = {});

// The report of what `import` wrote, from the report of its input: the
// written files, image file names (gltf::imageFileNames, by source image), and
// every index renumbered as `layout` renumbered it.
Report outputReport(const Report& input, const gltf::Layout& layout, const std::vector<gltf::WrittenFile>& files,
                    const std::vector<std::string>& image_names);

} // namespace meshtinker::analysis
