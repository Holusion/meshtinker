#include "meshtinker/common/outputs.hpp"

#include "meshtinker/common/failure.hpp"

#include <cerrno>
#include <cstdio>
#include <random>
#include <system_error>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

namespace meshtinker::common {

OutputFile::OutputFile(std::FILE* file, std::string name) : file_(file), name_(std::move(name)) {}

OutputFile::OutputFile(OutputFile&& other) noexcept
    : file_(std::exchange(other.file_, nullptr)), name_(std::move(other.name_)), position_(other.position_) {}

OutputFile::~OutputFile() {
    if (file_ != nullptr) {
        std::fclose(file_);
    }
}

void OutputFile::fail(int error_number, const char* operation) {
    throw systemFailure(error_number == 0 ? EIO : error_number, Side::machine, "output-failed",
                        std::string("cannot ") + operation + " " + name_);
}

void OutputFile::write(ByteView bytes) {
    if (bytes.size == 0) {
        return;
    }
    errno = 0;
    if (std::fwrite(bytes.data, 1, bytes.size, file_) != bytes.size) {
        fail(errno, "write");
    }
    position_ += bytes.size;
}

void OutputFile::write(const std::string& text) {
    write(ByteView{reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

void OutputFile::align(std::uint64_t alignment) {
    static const std::uint8_t zeros[16] = {};
    while (position_ % alignment != 0) {
        const std::uint64_t missing = alignment - position_ % alignment;
        write(ByteView{zeros, static_cast<std::size_t>(missing < sizeof zeros ? missing : sizeof zeros)});
    }
}

void OutputFile::close() {
    std::FILE* file = std::exchange(file_, nullptr);
    errno = 0;
    if (std::fclose(file) != 0) {
        fail(errno, "write");
    }
}

namespace {

std::string randomSuffix() {
    std::random_device device;
    const std::uint64_t value = (std::uint64_t{device()} << 32) | device();
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(value));
    return text;
}

// Moves `from` to `to` unless `to` exists. Returns 0 or an errno value.
int renameNoReplace(const std::filesystem::path& from, const std::filesystem::path& to) {
#if defined(__linux__) && defined(RENAME_NOREPLACE)
    if (::renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0) {
        return 0;
    }
    if (errno != EINVAL && errno != ENOSYS) {
        return errno;
    }
#endif
#if defined(__unix__) || defined(__APPLE__)
    // A hard link fails if the destination exists, atomically.
    if (::link(from.c_str(), to.c_str()) == 0) {
        ::unlink(from.c_str());
        return 0;
    }
    if (errno == EEXIST) {
        return EEXIST;
    }
#endif
    // File systems without hard links: check, then rename.
    std::error_code error;
    if (std::filesystem::exists(to, error) || error) {
        return error ? error.value() : EEXIST;
    }
    std::filesystem::rename(from, to, error);
    return error ? error.value() : 0;
}

// Moves the directory `from` to `to` unless `to` exists. Returns 0 or an
// errno value.
int renameDirectoryNoReplace(const std::filesystem::path& from, const std::filesystem::path& to) {
#if defined(__linux__) && defined(RENAME_NOREPLACE)
    if (::renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0) {
        return 0;
    }
    if (errno != EINVAL && errno != ENOSYS) {
        return errno;
    }
#elif defined(__APPLE__) && defined(RENAME_EXCL)
    if (::renamex_np(from.c_str(), to.c_str(), RENAME_EXCL) == 0) {
        return 0;
    }
    if (errno != ENOTSUP) {
        return errno;
    }
#endif
#if defined(__unix__) || defined(__APPLE__)
    // Claim the name with an empty directory, which mkdir creates only if the
    // name is free, then replace it: rename may replace an empty directory,
    // and this one is ours.
    if (::mkdir(to.c_str(), 0777) != 0) {
        return errno;
    }
    if (::rename(from.c_str(), to.c_str()) != 0) {
        const int error = errno;
        ::rmdir(to.c_str());
        return error;
    }
    return 0;
#else
    std::error_code error;
    if (std::filesystem::exists(to, error) || error) {
        return error ? error.value() : EEXIST;
    }
    std::filesystem::rename(from, to, error);
    return error ? error.value() : 0;
#endif
}

Failure existsFailure(const std::filesystem::path& path) {
    return Failure(ExitCode::machine, "output-exists", "'" + path.filename().u8string() + "' already exists");
}

std::filesystem::path temporaryName(const std::filesystem::path& path) {
    return path.parent_path() / (path.filename().u8string() + ".meshtinker-" + randomSuffix() + ".tmp");
}

#if defined(__unix__) || defined(__APPLE__)
// Copies `size` bytes of the open file `from` into the empty file `to`
// without reading them into memory: a reflink where the file system shares
// blocks (Btrfs, XFS), else copy_file_range. Returns 0, an errno value, or -1
// with nothing written if neither is supported.
int copyInKernel(int from, int to, std::uint64_t size) {
#if defined(__linux__)
#if defined(FICLONE)
    if (::ioctl(to, FICLONE, from) == 0) {
        return 0;
    }
#endif
    constexpr std::uint64_t max_chunk = std::uint64_t{1} << 30;
    std::uint64_t done = 0;
    while (done < size) {
        const std::uint64_t left = size - done;
        const ssize_t copied =
            ::copy_file_range(from, nullptr, to, nullptr, static_cast<std::size_t>(left < max_chunk ? left : max_chunk), 0);
        if (copied < 0) {
            if (done == 0 && (errno == EXDEV || errno == EINVAL || errno == ENOSYS || errno == EOPNOTSUPP)) {
                return -1;
            }
            return errno;
        }
        if (copied == 0) {
            // The source got shorter since it was mapped.
            return EIO;
        }
        done += static_cast<std::uint64_t>(copied);
    }
    return 0;
#else
    (void)from;
    (void)to;
    (void)size;
    return -1;
#endif
}
#endif

} // namespace

Outputs::~Outputs() {
    if (committed_) {
        return;
    }
    // Newest first, so a directory's files go before the directory.
    for (auto entry = entries_.rbegin(); entry != entries_.rend(); ++entry) {
        std::error_code ignored;
        const std::filesystem::path& path = entry->published ? entry->destination : entry->temporary;
        if (entry->kind == EntryKind::directory) {
            std::filesystem::remove_all(path, ignored);
        } else {
            std::filesystem::remove(path, ignored);
        }
    }
}

Outputs::Entry& Outputs::reserve(const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::exists(std::filesystem::symlink_status(path, error))) {
        throw existsFailure(path);
    }
    EntryKind kind = EntryKind::file;
    for (const Entry& entry : entries_) {
        if (entry.kind == EntryKind::directory && entry.temporary == path.parent_path()) {
            kind = EntryKind::inner_file;
        }
    }
    std::filesystem::path temporary = kind == EntryKind::inner_file ? path : temporaryName(path);
    std::string registered = temporary.string();
    entries_.push_back({path, std::move(temporary), CleanupRegistration(std::move(registered)), kind, false});
    return entries_.back();
}

OutputFile Outputs::create(const std::filesystem::path& path) {
    const std::string name = "'" + path.filename().u8string() + "'";
    const std::filesystem::path temporary = reserve(path).temporary;
    errno = 0;
    // "x": fail if the file exists (C11), so no file is ever replaced.
    std::FILE* file = std::fopen(temporary.c_str(), "wbx");
    if (file == nullptr) {
        const int error_number = errno;
        entries_.pop_back();
        throw systemFailure(error_number, Side::machine, "output-failed", "cannot create " + name);
    }
    // Large writes bypass the buffer; small ones (JSON, padding) are batched.
    std::setvbuf(file, nullptr, _IOFBF, std::size_t{1} << 20);
    return OutputFile(file, name);
}

std::filesystem::path Outputs::createDirectory(const std::filesystem::path& path) {
    Entry& entry = reserve(path);
    entry.kind = EntryKind::directory;
    const std::filesystem::path temporary = entry.temporary;
    std::error_code error;
    if (!std::filesystem::create_directory(temporary, error)) {
        entries_.pop_back();
        throw systemFailure(error ? error.value() : EEXIST, Side::machine, "output-failed",
                            "cannot create '" + path.filename().u8string() + "'");
    }
    return temporary;
}

void Outputs::linkFrom(const std::filesystem::path& directory) { link_from_ = directory; }

std::uint64_t Outputs::copy(const std::filesystem::path& source, ByteView bytes,
                            const std::filesystem::path& destination) {
    const std::string name = "'" + destination.filename().u8string() + "'";
#if defined(__unix__) || defined(__APPLE__)
    std::error_code error;
    if (link_from_ && std::filesystem::equivalent(source.parent_path(), *link_from_, error)) {
        const std::filesystem::path temporary = reserve(destination).temporary;
        if (::link(source.c_str(), temporary.c_str()) == 0) {
            ++linked_;
            return bytes.size;
        }
        // No hard links here (another file system, or none at all): copy.
        entries_.pop_back();
    }
    const int from = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
    struct stat status {};
    if (from >= 0 && ::fstat(from, &status) == 0 && S_ISREG(status.st_mode) &&
        static_cast<std::uint64_t>(status.st_size) == bytes.size) {
        const std::filesystem::path temporary = reserve(destination).temporary;
        const int to = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (to < 0) {
            const int error_number = errno;
            ::close(from);
            entries_.pop_back();
            throw systemFailure(error_number, Side::machine, "output-failed", "cannot create " + name);
        }
        int result = copyInKernel(from, to, bytes.size);
        ::close(from);
        if (::close(to) != 0 && result == 0) {
            result = errno;
        }
        if (result > 0) {
            throw systemFailure(result, Side::machine, "output-failed", "cannot write " + name);
        }
        if (result == 0) {
            return bytes.size;
        }
        // Not supported here: write the mapped bytes into the reserved file.
        std::FILE* file = std::fopen(temporary.c_str(), "wb");
        if (file == nullptr) {
            throw systemFailure(errno, Side::machine, "output-failed", "cannot create " + name);
        }
        std::setvbuf(file, nullptr, _IOFBF, std::size_t{1} << 20);
        OutputFile output(file, name);
        output.write(bytes);
        output.close();
        return bytes.size;
    }
    if (from >= 0) {
        ::close(from);
    }
#else
    (void)source;
#endif
    // The file can't be opened again or changed size since it was mapped: the
    // mapped bytes are what the command read, so write those.
    OutputFile output = create(destination);
    output.write(bytes);
    output.close();
    return bytes.size;
}

void Outputs::commit() {
    for (Entry& entry : entries_) {
        if (entry.kind == EntryKind::inner_file) {
            continue;
        }
        const int error = entry.kind == EntryKind::directory
                              ? renameDirectoryNoReplace(entry.temporary, entry.destination)
                              : renameNoReplace(entry.temporary, entry.destination);
        if (error == EEXIST || error == ENOTEMPTY) {
            throw existsFailure(entry.destination);
        }
        if (error != 0) {
            throw systemFailure(error, Side::machine, "output-failed",
                                "cannot publish '" + entry.destination.filename().u8string() + "'");
        }
        entry.published = true;
        if (entry.kind == EntryKind::directory) {
            // Its files moved with it.
            for (Entry& inner : entries_) {
                if (inner.kind == EntryKind::inner_file && inner.temporary.parent_path() == entry.temporary) {
                    inner.temporary = entry.destination / inner.temporary.filename();
                }
            }
        }
    }
    committed_ = true;
}

std::vector<std::filesystem::path> Outputs::paths() const {
    std::vector<std::filesystem::path> paths;
    paths.reserve(entries_.size());
    for (const Entry& entry : entries_) {
        paths.push_back(entry.destination);
    }
    return paths;
}

} // namespace meshtinker::common
