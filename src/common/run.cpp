#include "meshtinker/common/run.hpp"

#include "meshtinker/common/failure.hpp"

#include <filesystem>
#include <new>

namespace meshtinker::common {

FailureReport describeFailure(std::exception_ptr exception) {
    try {
        std::rethrow_exception(std::move(exception));
    } catch (const Failure& failure) {
        return {failure.exitCode(), failure.code(), failure.what()};
    } catch (const std::bad_alloc&) {
        return {ExitCode::resource_limit, "out-of-memory", "out of memory"};
    } catch (const std::filesystem::filesystem_error& error) {
        // what() would show whole paths; messages name files only.
        std::string message = error.code().message();
        if (!error.path1().empty()) {
            message += ": '" + error.path1().filename().u8string() + "'";
        }
        return {classifySystemError(error.code().value(), Side::machine), "file-system", message};
    } catch (const std::exception& error) {
        return {ExitCode::internal, "internal", std::string("internal error: ") + error.what()};
    } catch (...) {
        return {ExitCode::internal, "internal", "internal error"};
    }
}

} // namespace meshtinker::common
