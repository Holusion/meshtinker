#pragma once

#include "meshtinker/common/exit_code.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace meshtinker::common {

// A failure that ends a command. `code` is a stable identifier for JSON
// consumers, such as `invalid-json` or `reference-refused`; the message is
// English and names files, not their folders.
class Failure : public std::runtime_error {
public:
    Failure(ExitCode exit_code, std::string code, const std::string& message);

    ExitCode exitCode() const noexcept { return exit_code_; }
    const std::string& code() const noexcept { return code_; }

private:
    ExitCode exit_code_;
    std::string code_;
};

// The input can't be used, and retrying won't change that.
Failure rejected(std::string code, const std::string& message);

// The command line is wrong, whatever the input.
Failure usageError(std::string code, const std::string& message);

// Where an operating-system error happened, which decides its class.
enum class Side : std::uint8_t {
    // Reading the input or a file it references: a missing or unreadable-as-a-file
    // path means the input is wrong.
    input,
    // Writing outputs or workspaces, or anything else: the machine is at fault.
    machine,
};

// Classifies an errno value, after mesh2gltf's failure.rs: a full disk, quota or
// file-size limit is a resource limit wherever it happens; ENOENT, EISDIR,
// ENOTDIR, ENAMETOOLONG and ELOOP reject the input when reading it; anything
// else is the machine's problem.
ExitCode classifySystemError(int error_number, Side side);

// A Failure for errno value `error_number`. The message is `what: <strerror>`.
Failure systemFailure(int error_number, Side side, std::string code, const std::string& what);

} // namespace meshtinker::common
