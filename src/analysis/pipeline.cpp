#include "meshtinker/analysis/pipeline.hpp"

#include "meshtinker/analysis/analyze.hpp"

namespace meshtinker::analysis {

Inspection inspect(const std::filesystem::path& path, const gltf::ReadOptions& options, common::Workspace& workspace,
                   common::Diagnostics& diagnostics) {
    gltf::Document document = gltf::Document::read(path, options);
    gltf::Selection selection = gltf::selectScene(document);
    Report report = analyzeDocument(document, selection, diagnostics);
    analyzeData(document, selection, report, workspace, diagnostics);
    return {std::move(document), std::move(selection), std::move(report)};
}

} // namespace meshtinker::analysis
