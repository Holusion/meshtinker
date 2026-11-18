#pragma once

#include <cstdint>

namespace meshtinker::common {

// Process exit codes shared by every command. AGENTS.md documents them for users.
// Exit statuses fit in 8 bits on POSIX.
enum class ExitCode : std::uint8_t {
    success = 0,
    input_rejected = 1,
    usage = 2,
    resource_limit = 3,
    machine = 4,
    internal = 70,
};

constexpr int toInt(ExitCode code) { return static_cast<int>(code); }

} // namespace meshtinker::common
