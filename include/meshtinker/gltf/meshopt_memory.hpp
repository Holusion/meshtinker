#pragma once

#include "meshtinker/common/workspace.hpp"

namespace meshtinker::gltf {

// While it lives, meshoptimizer allocates as common::LargeArray does: on the
// heap within their shared budget (common::heap_array_limit), in scratch
// blocks of `workspace` beyond. Its simplifier and optimizers allocate tens of
// bytes per vertex, which would put a whole primitive's working set on the
// heap otherwise (docs/files-and-memory.md). meshopt_setAllocator is
// process-wide: one at a time, from one thread.
class MeshoptMemory {
public:
    explicit MeshoptMemory(common::Workspace& workspace);
    MeshoptMemory(const MeshoptMemory&) = delete;
    MeshoptMemory& operator=(const MeshoptMemory&) = delete;
    MeshoptMemory(MeshoptMemory&&) = delete;
    MeshoptMemory& operator=(MeshoptMemory&&) = delete;
    ~MeshoptMemory();
};

} // namespace meshtinker::gltf
