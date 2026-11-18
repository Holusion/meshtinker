#pragma once

#include "output.hpp"

#include "meshtinker/common/diagnostics.hpp"
#include "meshtinker/common/outputs.hpp"
#include "meshtinker/common/workspace.hpp"
#include "meshtinker/gltf/document.hpp"

#include <filesystem>
#include <string>

namespace meshtinker::placeholder {

constexpr unsigned default_resolution = 16;
// Below 2, every surface vertex collapses onto one cell's mean. Above 128,
// each thread's grid passes 70 MiB, for detail a placeholder doesn't need.
constexpr unsigned min_resolution = 2;
constexpr unsigned max_resolution = 128;

struct Options {
    unsigned resolution = default_resolution;
    // 0 for every hardware thread.
    unsigned threads = 0;
    gltf::ReadOptions read;
};

// Builds the placeholder of `input` and writes it as `<stem>.gltf` and
// `<stem>.bin` in `directory` (docs/placeholder.md).
Report build(const std::filesystem::path& input, const std::filesystem::path& directory, const std::string& stem,
             const Options& options, common::Workspace& workspace, common::Outputs& outputs,
             common::Diagnostics& diagnostics);

} // namespace meshtinker::placeholder
