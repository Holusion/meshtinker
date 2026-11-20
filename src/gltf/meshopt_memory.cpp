#include "meshtinker/gltf/meshopt_memory.hpp"

#include "meshtinker/common/large_array.hpp"

#include <meshoptimizer.h>

#include <cstdlib>
#include <map>
#include <new>

namespace meshtinker::gltf {
namespace {

common::Workspace* current = nullptr;

// Where each live allocation is: heap blocks with their size, for the budget
// they hold, and scratch blocks.
struct Allocations {
    std::map<void*, std::size_t> heap;
    std::map<void*, common::ScratchBlock> scratch;
};
Allocations* allocations = nullptr;

// meshoptimizer can't report a failure: a full disk or a failed allocation
// throws through it, as std::bad_alloc does from operator new. Its blocks
// share the heap budget of common::LargeArray.
void* allocate(size_t size) {
    std::atomic<std::uint64_t>& heap_bytes = common::largeArrayHeapBytes();
    if (current != nullptr && heap_bytes.fetch_add(size) + size > common::heap_array_limit) {
        heap_bytes.fetch_sub(size);
        common::ScratchBlock block = current->block(size);
        void* data = block.data();
        allocations->scratch[data] = std::move(block);
        return data;
    }
    void* data = std::malloc(size);
    if (data == nullptr) {
        if (current != nullptr) {
            heap_bytes.fetch_sub(size);
        }
        throw std::bad_alloc();
    }
    if (current != nullptr) {
        allocations->heap[data] = size;
    }
    return data;
}

void deallocate(void* data) {
    if (allocations != nullptr) {
        if (const auto found = allocations->scratch.find(data); found != allocations->scratch.end()) {
            allocations->scratch.erase(found);
            return;
        }
        if (const auto found = allocations->heap.find(data); found != allocations->heap.end()) {
            common::largeArrayHeapBytes().fetch_sub(found->second);
            allocations->heap.erase(found);
        }
    }
    std::free(data);
}

} // namespace

MeshoptMemory::MeshoptMemory(common::Workspace& workspace) {
    current = &workspace;
    allocations = new Allocations();
    meshopt_setAllocator(allocate, deallocate);
}

MeshoptMemory::~MeshoptMemory() {
    meshopt_setAllocator(::operator new, ::operator delete);
    delete allocations;
    allocations = nullptr;
    current = nullptr;
}

} // namespace meshtinker::gltf
