#include "meshtinker/common/outputs.hpp"

#include "meshtinker/common/failure.hpp"

#include <cerrno>
#include <cstdio>
#include <random>
#include <system_error>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
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

Failure existsFailure(const std::filesystem::path& path) {
    return Failure(ExitCode::machine, "output-exists", "'" + path.filename().u8string() + "' already exists");
}

} // namespace

Outputs::~Outputs() {
    if (committed_) {
        return;
    }
    for (const Entry& entry : entries_) {
        std::error_code ignored;
        std::filesystem::remove(entry.published ? entry.destination : entry.temporary, ignored);
    }
}

OutputFile Outputs::create(const std::filesystem::path& path) {
    const std::string name = "'" + path.filename().u8string() + "'";
    std::error_code error;
    if (std::filesystem::exists(std::filesystem::symlink_status(path, error))) {
        throw existsFailure(path);
    }
    std::filesystem::path temporary = path.parent_path() / (path.filename().u8string() + ".meshtinker-" + randomSuffix() + ".tmp");
    errno = 0;
    // "x": fail if the file exists (C11), so no file is ever replaced.
    std::FILE* file = std::fopen(temporary.c_str(), "wbx");
    if (file == nullptr) {
        throw systemFailure(errno, Side::machine, "output-failed", "cannot create " + name);
    }
    entries_.push_back({path, temporary, CleanupRegistration(temporary.string()), false});
    // Large writes bypass the buffer; small ones (JSON, padding) are batched.
    std::setvbuf(file, nullptr, _IOFBF, std::size_t{1} << 20);
    return OutputFile(file, name);
}

void Outputs::commit() {
    for (Entry& entry : entries_) {
        const int error = renameNoReplace(entry.temporary, entry.destination);
        if (error == EEXIST) {
            throw existsFailure(entry.destination);
        }
        if (error != 0) {
            throw systemFailure(error, Side::machine, "output-failed",
                                "cannot publish '" + entry.destination.filename().u8string() + "'");
        }
        entry.published = true;
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
