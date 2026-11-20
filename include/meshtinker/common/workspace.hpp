#pragma once

#include "meshtinker/common/cleanup.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace meshtinker::common {

// A writable, zero-filled file mapped into memory: large working sets, such as
// texture coverage maps, live here instead of on the heap. Its pages are
// backed by the file, so under memory pressure the kernel writes them out
// instead of swapping or failing.
class ScratchMapping {
public:
    ScratchMapping() = default;
    ScratchMapping(std::uint8_t* data, std::size_t size);
    ScratchMapping(ScratchMapping&& other) noexcept;
    ScratchMapping& operator=(ScratchMapping&& other) noexcept;
    ScratchMapping(const ScratchMapping&) = delete;
    ScratchMapping& operator=(const ScratchMapping&) = delete;
    ~ScratchMapping();

    std::uint8_t* data() const { return data_; }
    std::size_t size() const { return size_; }

private:
    void reset() noexcept;

    std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
};

// Part of a shared scratch file: many small working arrays share one file
// and one mapping, which is released when its last block is.
class ScratchBlock {
public:
    ScratchBlock() = default;
    ScratchBlock(std::shared_ptr<ScratchMapping> chunk, std::uint8_t* data) : chunk_(std::move(chunk)), data_(data) {}

    std::uint8_t* data() const { return data_; }

private:
    std::shared_ptr<ScratchMapping> chunk_;
    std::uint8_t* data_ = nullptr;
};

// A private directory for a command's temporary files, after mesh2gltf's
// disk.rs. It goes on a real disk: by default the output's directory, or the
// input's for read-only commands, since the OS temporary directory is often
// RAM-backed. `--tmp-dir` chooses another parent.
//
// The directory, `meshtinker-<pid>-<random>`, is created on first use, so a
// command that needs no scratch space never writes. It is removed when the
// Workspace is destroyed or the process is interrupted, unless `keep` is set
// (`--keep-tmp`), in which case its path is printed to stderr.
class Workspace {
public:
    Workspace(std::filesystem::path parent, bool keep);
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;
    Workspace(Workspace&&) = delete;
    Workspace& operator=(Workspace&&) = delete;
    ~Workspace();

    // A new zero-filled file of `size` bytes, mapped read-write. Unless the
    // workspace is kept, the file is unlinked as soon as it is mapped, so its
    // disk space comes back even if the process is killed. Throws a Failure
    // if the disk is full (exit code 3) or the directory can't be written (4).
    ScratchMapping scratch(const std::string& name, std::uint64_t size);

    // `size` zero-filled bytes, 16-byte aligned, in a scratch file shared with
    // other blocks: 64 MiB files, or one of its own for a block over 16 MiB.
    // Space isn't reused within a file: it comes back when every block in it
    // is released.
    ScratchBlock block(std::uint64_t size);

    // The directory, once created.
    const std::optional<std::filesystem::path>& path() const { return path_; }

private:
    const std::filesystem::path& directory();

    std::filesystem::path parent_;
    bool keep_;
    std::optional<std::filesystem::path> path_;
    std::unique_ptr<CleanupRegistration> cleanup_;
    std::shared_ptr<ScratchMapping> chunk_;
    std::uint64_t chunk_used_ = 0;
    std::uint64_t next_name_ = 0;
};

} // namespace meshtinker::common
