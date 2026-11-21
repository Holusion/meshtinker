#pragma once

#include "meshtinker/common/bytes.hpp"
#include "meshtinker/common/workspace.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <type_traits>
#include <vector>

namespace meshtinker::common {

// Working arrays go on the heap while they hold this much in all, and in
// workspace scratch files beyond (docs/files-and-memory.md). 256 MiB: 93 % of
// the local collection simplifies without scratch, and 4 to 8 commands run in
// parallel keep at most 1 to 2 GiB on the heap; large models still spill
// (docs/experiments/heap-budget-results.md).
#ifdef MESHTINKER_HEAP_ARRAY_LIMIT
inline constexpr std::uint64_t heap_array_limit = MESHTINKER_HEAP_ARRAY_LIMIT;
#else
inline constexpr std::uint64_t heap_array_limit = std::uint64_t{256} << 20;
#endif

// Bytes LargeArrays hold on the heap, in all.
std::atomic<std::uint64_t>& largeArrayHeapBytes();

// Measurements only (docs/experiments/heap-budget.md): with
// MESHTINKER_ARRAY_STATS defined, records the most the heap budget held, and
// writes it at exit to the file the environment variable
// MESHTINKER_ARRAY_STATS_FILE names. A no-op otherwise.
#ifdef MESHTINKER_ARRAY_STATS
void noteLargeArrayHeapBytes();
#else
inline void noteLargeArrayHeapBytes() {}
#endif

// A zero-filled array of `count` trivially copyable elements, for per-vertex
// or per-face working data: on the heap while all LargeArrays hold at most
// heap_array_limit bytes there, else in a scratch block of `workspace`.
template <typename T>
class LargeArray {
    static_assert(std::is_trivially_copyable_v<T>, "LargeArray holds plain data");

public:
    LargeArray() = default;
    LargeArray(Workspace& workspace, std::size_t count) : size_(count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::bad_alloc();
        }
        const std::uint64_t bytes = std::uint64_t{count} * sizeof(T);
        std::atomic<std::uint64_t>& heap_bytes = largeArrayHeapBytes();
        if (heap_bytes.fetch_add(bytes) + bytes <= heap_array_limit) {
            heap_.assign(count, T{});
            data_ = heap_.data();
            heap_held_ = bytes;
            noteLargeArrayHeapBytes();
        } else {
            heap_bytes.fetch_sub(bytes);
            block_ = workspace.block(bytes);
            data_ = reinterpret_cast<T*>(block_.data());
        }
    }
    LargeArray(LargeArray&& other) noexcept { *this = std::move(other); }
    LargeArray& operator=(LargeArray&& other) noexcept {
        if (this != &other) {
            release();
            heap_ = std::move(other.heap_);
            block_ = std::move(other.block_);
            data_ = other.data_;
            size_ = other.size_;
            heap_held_ = other.heap_held_;
            other.data_ = nullptr;
            other.size_ = 0;
            other.heap_held_ = 0;
        }
        return *this;
    }
    LargeArray(const LargeArray&) = delete;
    LargeArray& operator=(const LargeArray&) = delete;
    ~LargeArray() { release(); }

    T* data() { return data_; }
    const T* data() const { return data_; }
    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    T& operator[](std::size_t i) { return data_[i]; }
    const T& operator[](std::size_t i) const { return data_[i]; }
    T* begin() { return data_; }
    T* end() { return data_ + size_; }
    const T* begin() const { return data_; }
    const T* end() const { return data_ + size_; }

    // Keeps only the first `count` elements. The memory stays allocated.
    void shrink(std::size_t count) {
        if (count < size_) {
            size_ = count;
        }
    }

    ByteView bytes() const { return {reinterpret_cast<const std::uint8_t*>(data_), size_ * sizeof(T)}; }

private:
    void release() noexcept {
        largeArrayHeapBytes().fetch_sub(heap_held_);
        heap_held_ = 0;
        heap_ = {};
        block_ = {};
    }

    std::vector<T> heap_;
    ScratchBlock block_;
    std::uint64_t heap_held_ = 0;
    T* data_ = nullptr;
    std::size_t size_ = 0;
};

} // namespace meshtinker::common
