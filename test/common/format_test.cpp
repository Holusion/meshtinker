#include "meshtinker/common/format.hpp"
#include "support/check.hpp"

#include <limits>
#include <string>

using meshtinker::common::formatBytes;
using meshtinker::common::formatDuration;

TEST_CASE(bytes_use_decimal_units) {
    CHECK_EQ(formatBytes(0), std::string("0 B"));
    CHECK_EQ(formatBytes(999), std::string("999 B"));
    CHECK_EQ(formatBytes(1000), std::string("1.0 kB"));
    CHECK_EQ(formatBytes(12345), std::string("12.3 kB"));
    CHECK_EQ(formatBytes(45'600'000), std::string("45.6 MB"));
    CHECK_EQ(formatBytes(1'234'000'000), std::string("1.23 GB"));
    CHECK_EQ(formatBytes(5'000'000'000'000), std::string("5000.00 GB"));
}

// A figure that rounds to 1000 takes the next unit.
TEST_CASE(bytes_round_into_the_next_unit) {
    CHECK_EQ(formatBytes(999'940), std::string("999.9 kB"));
    CHECK_EQ(formatBytes(999'960), std::string("1.0 MB"));
    CHECK_EQ(formatBytes(999'960'000), std::string("1.00 GB"));
}

TEST_CASE(durations_pick_their_unit) {
    CHECK_EQ(formatDuration(0), std::string("0 ms"));
    CHECK_EQ(formatDuration(0.0504), std::string("50 ms"));
    CHECK_EQ(formatDuration(0.9994), std::string("999 ms"));
    CHECK_EQ(formatDuration(0.9996), std::string("1.0 s"));
    CHECK_EQ(formatDuration(12.34), std::string("12.3 s"));
    CHECK_EQ(formatDuration(59.94), std::string("59.9 s"));
    CHECK_EQ(formatDuration(59.96), std::string("1 min 0 s"));
    CHECK_EQ(formatDuration(83.4), std::string("1 min 23 s"));
    CHECK_EQ(formatDuration(3599.4), std::string("59 min 59 s"));
    CHECK_EQ(formatDuration(3599.6), std::string("1 h 00 min"));
    CHECK_EQ(formatDuration(2 * 3600 + 5 * 60 + 10), std::string("2 h 05 min"));
}

TEST_CASE(invalid_durations_read_zero) {
    CHECK_EQ(formatDuration(-1), std::string("0 ms"));
    CHECK_EQ(formatDuration(std::numeric_limits<double>::quiet_NaN()), std::string("0 ms"));
    CHECK_EQ(formatDuration(std::numeric_limits<double>::infinity()), std::string("0 ms"));
}
