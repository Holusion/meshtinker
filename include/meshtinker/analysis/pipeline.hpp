#pragma once

#include "meshtinker/analysis/analyze.hpp"
#include "meshtinker/analysis/report.hpp"
#include "meshtinker/common/diagnostics.hpp"
#include "meshtinker/common/workspace.hpp"
#include "meshtinker/gltf/document.hpp"
#include "meshtinker/gltf/selection.hpp"

#include <filesystem>

namespace meshtinker::analysis {

// What `inspect` reports and `import` starts from: the document, the part of
// it the normalized profile keeps, and its statistics. Both commands call
// this, so they accept and reject the same inputs.
struct Inspection {
    gltf::Document document;
    gltf::Selection selection;
    Report report;
};

// Reads `path`, selects its scene, then runs both analysis stages.
Inspection inspect(const std::filesystem::path& path, const gltf::ReadOptions& options, common::Workspace& workspace,
                   common::Diagnostics& diagnostics, const CoverageSink& maps = {});

} // namespace meshtinker::analysis
