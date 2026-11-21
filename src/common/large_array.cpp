#include "meshtinker/common/large_array.hpp"

#include <cstdio>
#include <cstdlib>

namespace meshtinker::common {

std::atomic<std::uint64_t>& largeArrayHeapBytes() {
    static std::atomic<std::uint64_t> bytes{0};
    return bytes;
}

#ifdef MESHTINKER_ARRAY_STATS
namespace {

std::atomic<std::uint64_t>& peakBytes() {
    static std::atomic<std::uint64_t> peak{0};
    return peak;
}

struct PeakWriter {
    PeakWriter(const PeakWriter&) = delete;
    PeakWriter& operator=(const PeakWriter&) = delete;
    PeakWriter() = default;
    ~PeakWriter() {
        if (const char* path = std::getenv("MESHTINKER_ARRAY_STATS_FILE")) {
            if (std::FILE* file = std::fopen(path, "w")) {
                std::fprintf(file, "%llu\n", static_cast<unsigned long long>(peakBytes().load()));
                std::fclose(file);
            }
        }
    }
};
PeakWriter writer;

} // namespace

void noteLargeArrayHeapBytes() {
    const std::uint64_t now = largeArrayHeapBytes().load();
    std::uint64_t peak = peakBytes().load();
    while (now > peak && !peakBytes().compare_exchange_weak(peak, now)) {
    }
}
#endif

} // namespace meshtinker::common
