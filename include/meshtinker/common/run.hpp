#pragma once

#include "meshtinker/common/exit_code.hpp"

#include <exception>
#include <string>

namespace meshtinker::common {

// What ended a command, for its error report and exit code.
struct FailureReport {
    ExitCode exit_code;
    // Stable identifier, as in Failure::code().
    std::string code;
    std::string message;
};

// Classifies an exception escaping a command:
// - common::Failure carries its own class;
// - std::bad_alloc is a resource limit (`out-of-memory`);
// - std::filesystem::filesystem_error is classified as a machine-side system error;
// - anything else is an internal error (exit code 70): a bug in meshtinker.
FailureReport describeFailure(std::exception_ptr exception);

} // namespace meshtinker::common
