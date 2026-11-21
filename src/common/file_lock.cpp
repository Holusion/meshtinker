#include "meshtinker/common/file_lock.hpp"

#include "meshtinker/common/failure.hpp"

#include <cerrno>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace meshtinker::common {

std::optional<FileLock> FileLock::tryAcquire(const std::filesystem::path& path) {
#if defined(__unix__) || defined(__APPLE__)
    bool created = true;
    int descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (descriptor < 0 && errno == EEXIST) {
        created = false;
        descriptor = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    }
    if (descriptor < 0) {
        throw systemFailure(errno, Side::machine, "lock-failed", "cannot open '" + path.filename().u8string() + "'");
    }
    if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        // The file stays even if this call created it: another process that
        // opened it meanwhile may hold the lock, and removing the file would
        // let a third one lock a new file of the same name.
        const int error_number = errno;
        ::close(descriptor);
        if (error_number == EWOULDBLOCK) {
            return std::nullopt;
        }
        throw systemFailure(error_number, Side::machine, "lock-failed", "cannot lock '" + path.filename().u8string() + "'");
    }
    return FileLock(descriptor, created);
#else
    (void)path;
    return FileLock(-1, false);
#endif
}

FileLock::FileLock(FileLock&& other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1)), created_(other.created_) {}

FileLock::~FileLock() {
#if defined(__unix__) || defined(__APPLE__)
    if (descriptor_ >= 0) {
        ::close(descriptor_);
    }
#endif
}

} // namespace meshtinker::common
