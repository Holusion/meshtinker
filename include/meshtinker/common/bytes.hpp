#pragma once

#include <cstddef>
#include <cstdint>

namespace meshtinker::common {

// A read-only range of bytes owned elsewhere, usually a mapped file.
struct ByteView {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;

    bool empty() const { return size == 0; }

    // The `length` bytes at `offset`, or an empty view if they don't fit.
    ByteView sub(std::uint64_t offset, std::uint64_t length) const {
        if (offset > size || length > size - offset) {
            return {};
        }
        return {data + offset, static_cast<std::size_t>(length)};
    }

    // True if `length` bytes at `offset` are inside the view.
    bool contains(std::uint64_t offset, std::uint64_t length) const {
        return offset <= size && length <= size - offset;
    }
};

} // namespace meshtinker::common
