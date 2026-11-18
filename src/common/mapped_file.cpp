#include "meshtinker/common/mapped_file.hpp"

#include <cerrno>
#include <limits>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#define MESHTINKER_MMAP 1
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <fstream>
#include <new>
#endif

namespace meshtinker::common {

MappedFile::MappedFile(MappedFile&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)),
      mapped_(std::exchange(other.mapped_, false)) {}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        reset();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
        mapped_ = std::exchange(other.mapped_, false);
    }
    return *this;
}

MappedFile::~MappedFile() { reset(); }

#if defined(MESHTINKER_MMAP)

MappedFile MappedFile::open(const std::filesystem::path& path, Side side, const std::string& name) {
    // O_NONBLOCK keeps a FIFO from blocking the open; the type check follows.
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (descriptor < 0) {
        throw systemFailure(errno, side, "file-unreadable", "cannot open " + name);
    }
    struct stat status {};
    if (::fstat(descriptor, &status) != 0) {
        const int error = errno;
        ::close(descriptor);
        throw systemFailure(error, side, "file-unreadable", "cannot read " + name);
    }
    if (!S_ISREG(status.st_mode)) {
        ::close(descriptor);
        throw Failure(side == Side::input ? ExitCode::input_rejected : ExitCode::machine, "not-a-file",
                      name + " is not a regular file");
    }
    const auto file_size = static_cast<std::uint64_t>(status.st_size);
    if (file_size > std::numeric_limits<std::size_t>::max()) {
        ::close(descriptor);
        throw Failure(ExitCode::resource_limit, "file-too-large", name + " is too large to map");
    }
    MappedFile file;
    if (file_size > 0) {
        void* address = ::mmap(nullptr, static_cast<std::size_t>(file_size), PROT_READ, MAP_PRIVATE, descriptor, 0);
        if (address == MAP_FAILED) {
            const int error = errno;
            ::close(descriptor);
            throw systemFailure(error, Side::machine, "file-unreadable", "cannot map " + name);
        }
        file.data_ = static_cast<const std::uint8_t*>(address);
        file.size_ = static_cast<std::size_t>(file_size);
        file.mapped_ = true;
    }
    ::close(descriptor);
    return file;
}

void MappedFile::reset() noexcept {
    if (mapped_) {
        ::munmap(const_cast<std::uint8_t*>(data_), size_);
    } else {
        delete[] data_;
    }
    data_ = nullptr;
    size_ = 0;
    mapped_ = false;
}

void MappedFile::releasePages() const noexcept {
    if (mapped_) {
        // glibc's posix_madvise ignores POSIX_MADV_DONTNEED; madvise drops the
        // clean pages of this read-only mapping, which later reads fault in again.
        ::madvise(const_cast<std::uint8_t*>(data_), size_, MADV_DONTNEED);
    }
}

#else

MappedFile MappedFile::open(const std::filesystem::path& path, Side side, const std::string& name) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        throw Failure(side == Side::input ? ExitCode::input_rejected : ExitCode::machine, "not-a-file",
                      name + " is not a regular file");
    }
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        throw Failure(ExitCode::machine, "file-unreadable", "cannot open " + name);
    }
    const auto file_size = static_cast<std::uint64_t>(stream.tellg());
    MappedFile file;
    if (file_size > 0) {
        auto* buffer = new std::uint8_t[file_size];
        stream.seekg(0);
        if (!stream.read(reinterpret_cast<char*>(buffer), static_cast<std::streamsize>(file_size))) {
            delete[] buffer;
            throw Failure(ExitCode::machine, "file-unreadable", "cannot read " + name);
        }
        file.data_ = buffer;
        file.size_ = static_cast<std::size_t>(file_size);
    }
    return file;
}

void MappedFile::reset() noexcept {
    delete[] data_;
    data_ = nullptr;
    size_ = 0;
}

void MappedFile::releasePages() const noexcept {}

#endif

} // namespace meshtinker::common
