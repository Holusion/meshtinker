#pragma once

#include "meshtinker/common/bytes.hpp"
#include "meshtinker/common/failure.hpp"

#include <cstdint>
#include <filesystem>

namespace meshtinker::common {

// A whole file mapped read-only. Its pages are backed by the file, so they
// don't count as heap and the kernel can drop them under memory pressure.
// The file must not be truncated while it is mapped. Platforms without mmap
// read the file into memory instead.
class MappedFile {
public:
    // Maps `path`, which must be a regular file. Errors are classified for `side`.
    // `name` is how messages refer to the file.
    static MappedFile open(const std::filesystem::path& path, Side side, const std::string& name);

    MappedFile() = default;
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    ~MappedFile();

    ByteView bytes() const { return {data_, size_}; }
    std::uint64_t size() const { return size_; }

    // Hints that the pages read so far are not needed any more. They stay
    // valid and are read again from the file on the next access.
    void releasePages() const noexcept;

private:
    void reset() noexcept;

    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    // True when data_ comes from mmap, false when it is heap memory or empty.
    bool mapped_ = false;
};

} // namespace meshtinker::common
