#include "meshtinker/common/large_array.hpp"

namespace meshtinker::common {

std::atomic<std::uint64_t>& largeArrayHeapBytes() {
    static std::atomic<std::uint64_t> bytes{0};
    return bytes;
}

} // namespace meshtinker::common
