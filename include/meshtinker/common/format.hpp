#pragma once

#include <cstdint>
#include <string>

namespace meshtinker::common {

// Sizes and times for people to read, in text reports and logs. JSON keeps the
// raw figures (bytes, seconds), for consumers to format their own way.

// Decimal units, as file managers and download pages show sizes: `999 B`,
// `12.3 kB`, `45.6 MB`, `1.23 GB`.
std::string formatBytes(std::uint64_t bytes);

// `850 ms`, `12.3 s`, `1 min 23 s`, `2 h 05 min`. Negative or non-finite
// durations read `0 ms`.
std::string formatDuration(double seconds);

} // namespace meshtinker::common
