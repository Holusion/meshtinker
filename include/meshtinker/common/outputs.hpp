#pragma once

#include "meshtinker/common/bytes.hpp"
#include "meshtinker/common/cleanup.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace meshtinker::common {

// A new file being written, buffered. Write errors throw a Failure classified
// on the machine side, so a full disk is a resource limit.
class OutputFile {
public:
    OutputFile(std::FILE* file, std::string name);
    OutputFile(OutputFile&& other) noexcept;
    OutputFile& operator=(OutputFile&&) = delete;
    OutputFile(const OutputFile&) = delete;
    OutputFile& operator=(const OutputFile&) = delete;
    ~OutputFile();

    void write(ByteView bytes);
    void write(const std::string& text);
    // Writes zero bytes until the position is a multiple of `alignment`.
    void align(std::uint64_t alignment);
    std::uint64_t position() const { return position_; }
    // Flushes and closes the file, reporting any error. The destructor closes
    // without reporting, for error paths.
    void close();

private:
    void fail(int error_number, const char* operation);

    std::FILE* file_;
    std::string name_;
    std::uint64_t position_ = 0;
};

// The files a command writes, published only when it succeeds, after
// mesh2gltf's disk.rs.
//
// create() refuses a destination that exists, then writes a temporary file
// beside it, so publishing is a rename on the same file system. It is named
// `<name>.meshtinker-<random>.tmp` and not hidden, so a killed run's leftovers
// are easy to find, and `*.meshtinker-*.tmp` matches only them. commit() moves
// every temporary file to its destination, never replacing a file that
// appeared meanwhile. Without commit(), or if it fails,
// the temporary files and anything already published are removed: a failed
// run leaves nothing behind and never touches files it didn't create.
// Temporary files are also removed if the process is interrupted (cleanup.hpp).
class Outputs {
public:
    Outputs() = default;
    Outputs(const Outputs&) = delete;
    Outputs& operator=(const Outputs&) = delete;
    Outputs(Outputs&&) = delete;
    Outputs& operator=(Outputs&&) = delete;
    ~Outputs();

    // Starts writing `path`. Throws a Failure if it exists (`output-exists`,
    // exit code 4) or its temporary file can't be created.
    OutputFile create(const std::filesystem::path& path);
    // Publishes every file created so far. Every OutputFile must be closed.
    void commit();
    // The destinations, in creation order.
    std::vector<std::filesystem::path> paths() const;

private:
    struct Entry {
        std::filesystem::path destination;
        std::filesystem::path temporary;
        CleanupRegistration cleanup;
        bool published = false;
    };
    std::vector<Entry> entries_;
    bool committed_ = false;
};

} // namespace meshtinker::common
