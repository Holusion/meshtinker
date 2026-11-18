#include "meshtinker/common/failure.hpp"

#include <cerrno>
#include <cstring>
#include <utility>

namespace meshtinker::common {

Failure::Failure(ExitCode exit_code, std::string code, const std::string& message)
    : std::runtime_error(message), exit_code_(exit_code), code_(std::move(code)) {}

Failure rejected(std::string code, const std::string& message) {
    return Failure(ExitCode::input_rejected, std::move(code), message);
}

Failure usageError(std::string code, const std::string& message) {
    return Failure(ExitCode::usage, std::move(code), message);
}

ExitCode classifySystemError(int error_number, Side side) {
    switch (error_number) {
    case ENOSPC:
    case EFBIG:
#if defined(EDQUOT)
    case EDQUOT:
#endif
        return ExitCode::resource_limit;
    case ENOENT:
    case EISDIR:
    case ENOTDIR:
    case ENAMETOOLONG:
#if defined(ELOOP)
    case ELOOP:
#endif
        return side == Side::input ? ExitCode::input_rejected : ExitCode::machine;
    default:
        return ExitCode::machine;
    }
}

Failure systemFailure(int error_number, Side side, std::string code, const std::string& what) {
    return Failure(classifySystemError(error_number, side), std::move(code),
                   what + ": " + std::strerror(error_number));
}

} // namespace meshtinker::common
