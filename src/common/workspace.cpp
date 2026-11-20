#include "meshtinker/common/workspace.hpp"

#include "meshtinker/common/failure.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <iostream>
#include <memory>
#include <limits>
#include <random>
#include <system_error>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#define MESHTINKER_MMAP 1
#endif

namespace meshtinker::common {
namespace fs = std::filesystem;

ScratchMapping::ScratchMapping(std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

ScratchMapping::ScratchMapping(ScratchMapping&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}

ScratchMapping& ScratchMapping::operator=(ScratchMapping&& other) noexcept {
    if (this != &other) {
        reset();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

ScratchMapping::~ScratchMapping() { reset(); }

void ScratchMapping::reset() noexcept {
#if defined(MESHTINKER_MMAP)
    if (data_ != nullptr && size_ > 0) {
        ::munmap(data_, size_);
    }
#else
    delete[] data_;
#endif
    data_ = nullptr;
    size_ = 0;
}

Workspace::Workspace(fs::path parent, bool keep) : parent_(std::move(parent)), keep_(keep) {
    if (parent_.empty()) {
        parent_ = ".";
    }
}

Workspace::~Workspace() {
    if (!path_) {
        return;
    }
    if (keep_) {
        std::cerr << "Kept workspace: " << path_->string() << '\n';
        return;
    }
    std::error_code ignored;
    fs::remove_all(*path_, ignored);
}

namespace {

long processId() {
#if defined(MESHTINKER_MMAP)
    return static_cast<long>(::getpid());
#else
    return 0;
#endif
}

} // namespace

const fs::path& Workspace::directory() {
    if (path_) {
        return *path_;
    }
    std::random_device device;
    char suffix[17];
    std::snprintf(suffix, sizeof suffix, "%08x%08x", device(), device());
    // The process ID alone repeats across PID namespaces (containers); the
    // random part keeps a killed run's leftovers from blocking the next one.
    fs::path path = parent_ / ("meshtinker-" + std::to_string(processId()) + "-" + suffix);
    std::error_code error;
    if (!fs::create_directory(path, error) || error) {
        throw systemFailure(error ? error.value() : EEXIST, Side::machine, "workspace-failed",
                            "cannot create a workspace in " + parent_.string() + " (choose another with --tmp-dir)");
    }
    if (!keep_) {
        cleanup_ = std::make_unique<CleanupRegistration>(path.string());
    }
    path_ = std::move(path);
    return *path_;
}

ScratchBlock Workspace::block(std::uint64_t size) {
    constexpr std::uint64_t chunk_size = std::uint64_t{64} << 20;
    constexpr std::uint64_t own_chunk = std::uint64_t{16} << 20;
    constexpr std::uint64_t alignment = 16;
    size = (size + alignment - 1) / alignment * alignment;
    if (size > own_chunk) {
        auto chunk = std::make_shared<ScratchMapping>(scratch("block-" + std::to_string(next_name_++), size));
        std::uint8_t* data = chunk->data();
        return {std::move(chunk), data};
    }
    if (size == 0) {
        return {};
    }
    if (!chunk_ || chunk_used_ + size > chunk_->size()) {
        chunk_ = std::make_shared<ScratchMapping>(scratch("blocks-" + std::to_string(next_name_++), chunk_size));
        chunk_used_ = 0;
    }
    std::uint8_t* data = chunk_->data() + chunk_used_;
    chunk_used_ += size;
    return {chunk_, data};
}

ScratchMapping Workspace::scratch(const std::string& name, std::uint64_t size) {
    // Mappable, and a valid 64-bit file offset.
    constexpr std::uint64_t limit = std::min<std::uint64_t>(std::numeric_limits<std::size_t>::max(),
                                                            std::numeric_limits<std::int64_t>::max());
    if (size > limit) {
        throw Failure(ExitCode::resource_limit, "workspace-too-large", "scratch file " + name + " is too large");
    }
    if (size == 0) {
        return {};
    }
    const fs::path path = directory() / name;
#if defined(MESHTINKER_MMAP)
    const int descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        throw systemFailure(errno, Side::machine, "workspace-failed", "cannot create scratch file " + name);
    }
    if (!keep_) {
        ::unlink(path.c_str());
    }
    // posix_fallocate reserves the blocks now, so a full disk fails here and
    // not as SIGBUS when a page is first written. Some file systems don't
    // support it (EINVAL, EOPNOTSUPP); ftruncate then makes a sparse file.
    int error = ::posix_fallocate(descriptor, 0, static_cast<off_t>(size));
    if (error == EINVAL || error == EOPNOTSUPP) {
        error = ::ftruncate(descriptor, static_cast<off_t>(size)) == 0 ? 0 : errno;
    }
    if (error != 0) {
        ::close(descriptor);
        throw systemFailure(error, Side::machine, "workspace-failed", "cannot allocate scratch file " + name);
    }
    void* address = ::mmap(nullptr, static_cast<std::size_t>(size), PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
    const int map_error = errno;
    ::close(descriptor);
    if (address == MAP_FAILED) {
        throw systemFailure(map_error, Side::machine, "workspace-failed", "cannot map scratch file " + name);
    }
    return ScratchMapping(static_cast<std::uint8_t*>(address), static_cast<std::size_t>(size));
#else
    (void)path;
    return ScratchMapping(new std::uint8_t[size](), static_cast<std::size_t>(size));
#endif
}

} // namespace meshtinker::common
