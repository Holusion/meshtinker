#include "meshtinker/common/process.hpp"

#include "meshtinker/common/cleanup.hpp"
#include "meshtinker/common/failure.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <system_error>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace meshtinker::common {
namespace fs = std::filesystem;

namespace {

fs::path canonicalOrEmpty(const fs::path& path) {
    std::error_code error;
    fs::path result = fs::canonical(path, error);
    return error ? fs::path() : result;
}

fs::path osExecutablePath() {
#if defined(__linux__)
    return canonicalOrEmpty("/proc/self/exe");
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    return canonicalOrEmpty(buffer.c_str());
#else
    return {};
#endif
}

} // namespace

fs::path currentExecutable(const char* argv0) {
    if (fs::path path = osExecutablePath(); !path.empty()) {
        return path;
    }
    if (argv0 == nullptr || *argv0 == '\0') {
        return {};
    }
    const fs::path invoked(argv0);
    if (invoked.has_parent_path()) {
        return canonicalOrEmpty(invoked);
    }
    for (const fs::path& directory : searchPath()) {
        if (isExecutableFile(directory / invoked)) {
            return canonicalOrEmpty(directory / invoked);
        }
    }
    return {};
}

bool isExecutableFile(const fs::path& path) {
    std::error_code error;
    if (!fs::is_regular_file(path, error)) {
        return false;
    }
#if defined(_WIN32)
    return true;
#else
    return ::access(path.c_str(), X_OK) == 0;
#endif
}

std::vector<fs::path> searchPath() {
    std::vector<fs::path> directories;
    const char* value = std::getenv("PATH");
    if (value == nullptr) {
        return directories;
    }
#if defined(_WIN32)
    const char separator = ';';
#else
    const char separator = ':';
#endif
    const std::string path(value);
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find(separator, start);
        if (end == std::string::npos) {
            end = path.size();
        }
        if (end > start) {
            directories.emplace_back(path.substr(start, end - start));
        }
        start = end + 1;
    }
    return directories;
}

std::string replaceProcess(const fs::path& program, const std::vector<std::string>& arguments) {
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const std::string& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
#if defined(_WIN32)
    // Windows has no exec: run the program, wait, and exit with its status.
    const intptr_t status = _spawnv(_P_WAIT, program.string().c_str(), argv.data());
    if (status != -1) {
        std::exit(static_cast<int>(status));
    }
#else
    ::execv(program.c_str(), argv.data());
#endif
    return std::strerror(errno);
}

std::optional<std::string> captureOutput(const fs::path& program, const std::vector<std::string>& arguments) {
#if defined(_WIN32)
    (void)program;
    (void)arguments;
    return std::nullopt;
#else
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const std::string& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    int pipe_ends[2];
    if (::pipe(pipe_ends) != 0) {
        return std::nullopt;
    }
    const pid_t child = ::fork();
    if (child < 0) {
        ::close(pipe_ends[0]);
        ::close(pipe_ends[1]);
        return std::nullopt;
    }
    if (child == 0) {
        const int null_device = ::open("/dev/null", O_RDWR);
        ::dup2(null_device, STDIN_FILENO);
        ::dup2(null_device, STDERR_FILENO);
        ::dup2(pipe_ends[1], STDOUT_FILENO);
        ::close(pipe_ends[0]);
        ::execv(program.c_str(), argv.data());
        ::_exit(127);
    }
    ::close(pipe_ends[1]);
    std::string output;
    char buffer[4096];
    for (;;) {
        const ssize_t got = ::read(pipe_ends[0], buffer, sizeof buffer);
        if (got > 0) {
            output.append(buffer, static_cast<std::size_t>(got));
        } else if (got == 0 || errno != EINTR) {
            break;
        }
    }
    ::close(pipe_ends[0]);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return std::nullopt;
    }
    return output;
#endif
}

ChildResult runChild(const fs::path& program, const std::vector<std::string>& arguments) {
#if defined(_WIN32)
    (void)arguments;
    throw Failure(ExitCode::machine, "cannot-run", "Can't run " + program.filename().string() + " on this platform");
#else
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const std::string& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    int pipe_ends[2];
    if (::pipe(pipe_ends) != 0) {
        throw Failure(ExitCode::machine, "cannot-run", "Can't run " + program.filename().string() + ": " + std::strerror(errno));
    }
    const pid_t child = ::fork();
    if (child < 0) {
        ::close(pipe_ends[0]);
        ::close(pipe_ends[1]);
        throw Failure(ExitCode::machine, "cannot-run", "Can't run " + program.filename().string() + ": " + std::strerror(errno));
    }
    if (child == 0) {
        const int null_device = ::open("/dev/null", O_RDONLY);
        ::dup2(null_device, STDIN_FILENO);
        ::dup2(pipe_ends[1], STDOUT_FILENO);
        ::close(pipe_ends[0]);
        ::close(pipe_ends[1]);
        ::execv(program.c_str(), argv.data());
        ::_exit(127);
    }
    setForwardedChild(static_cast<long>(child));
    ::close(pipe_ends[1]);
    ChildResult result;
    char buffer[65536];
    for (;;) {
        const ssize_t got = ::read(pipe_ends[0], buffer, sizeof buffer);
        if (got > 0) {
            result.output.append(buffer, static_cast<std::size_t>(got));
        } else if (got == 0 || errno != EINTR) {
            break;
        }
    }
    ::close(pipe_ends[0]);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    setForwardedChild(0);
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.signal = WTERMSIG(status);
    }
    return result;
#endif
}

} // namespace meshtinker::common
