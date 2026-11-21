#pragma once

#include <filesystem>
#include <optional>

namespace meshtinker::common {

// An exclusive advisory lock (`flock`) on a file, held for the object's
// lifetime. The kernel releases it when the process dies, however it dies, so
// no stale lock is ever left behind.
class FileLock {
public:
    // Locks `path`, creating it if needed. Returns nothing if another process
    // holds the lock: callers fail at once rather than wait. Throws a Failure
    // (exit code 4, `lock-failed`) if the file can't be opened.
    static std::optional<FileLock> tryAcquire(const std::filesystem::path& path);

    FileLock(FileLock&& other) noexcept;
    FileLock& operator=(FileLock&&) = delete;
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    ~FileLock();

    // Whether tryAcquire created the file.
    bool created() const { return created_; }

private:
    FileLock(int descriptor, bool created) : descriptor_(descriptor), created_(created) {}

    int descriptor_ = -1;
    bool created_ = false;
};

} // namespace meshtinker::common
