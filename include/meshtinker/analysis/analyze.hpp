#pragma once

#include "meshtinker/analysis/report.hpp"
#include "meshtinker/common/diagnostics.hpp"
#include "meshtinker/common/workspace.hpp"
#include "meshtinker/gltf/document.hpp"
#include "meshtinker/gltf/layout.hpp"
#include "meshtinker/gltf/selection.hpp"
#include "meshtinker/gltf/writer.hpp"

#include <vector>

namespace meshtinker::analysis {

// Stage 1, document fields: what the JSON, file sizes and image headers say.
// Everything except bounds and occupancy (docs/inspect-report.md, "Cost").
// `inspect --no-data` stops here, on a document read without its files.
Report analyzeDocument(const gltf::Document& document, const gltf::Selection& selection,
                       common::Diagnostics& diagnostics);

// Stage 2, data fields: reads vertex data for the scene's bounds, and
// rasterizes UV triangles for texture occupancy. Large working sets go in
// `workspace`.
void analyzeData(gltf::Document& document, const gltf::Selection& selection, Report& report,
                 common::Workspace& workspace, common::Diagnostics& diagnostics);

// The report of what `import` wrote, from the report of its input: the
// written files, image file names (gltf::imageFileNames, by source image), and
// every index renumbered as `layout` renumbered it.
Report outputReport(const Report& input, const gltf::Layout& layout, const std::vector<gltf::WrittenFile>& files,
                    const std::vector<std::string>& image_names);

} // namespace meshtinker::analysis
