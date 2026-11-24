#include "meshtinker/common/format.hpp"

#include <cmath>
#include <cstdio>

namespace meshtinker::common {

std::string formatBytes(std::uint64_t bytes) {
    char text[32];
    if (bytes < 1000) {
        std::snprintf(text, sizeof text, "%llu B", static_cast<unsigned long long>(bytes));
        return text;
    }
    // Each unit while its rounded figure stays under 1000, so that 999 960
    // bytes read `1.0 MB`, not `1000.0 kB`. GB has two decimals and no unit
    // after it.
    const double value = static_cast<double>(bytes);
    if (std::round(value / 1e2) < 1e4) {
        std::snprintf(text, sizeof text, "%.1f kB", value / 1e3);
    } else if (std::round(value / 1e5) < 1e4) {
        std::snprintf(text, sizeof text, "%.1f MB", value / 1e6);
    } else {
        std::snprintf(text, sizeof text, "%.2f GB", value / 1e9);
    }
    return text;
}

std::string formatDuration(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0) {
        seconds = 0;
    }
    char text[48];
    // Each form while its rounded figure stays in range, so that 59.96 s reads
    // `1 min 0 s`, not `60.0 s`.
    if (std::round(seconds * 1e3) < 1e3) {
        std::snprintf(text, sizeof text, "%.0f ms", std::round(seconds * 1e3));
    } else if (std::round(seconds * 10) < 600) {
        std::snprintf(text, sizeof text, "%.1f s", std::round(seconds * 10) / 10);
    } else if (std::round(seconds) < 3600) {
        const auto total = static_cast<long long>(std::round(seconds));
        std::snprintf(text, sizeof text, "%lld min %lld s", total / 60, total % 60);
    } else {
        const auto total = static_cast<long long>(std::round(seconds / 60));
        std::snprintf(text, sizeof text, "%lld h %02lld min", total / 60, total % 60);
    }
    return text;
}

} // namespace meshtinker::common
