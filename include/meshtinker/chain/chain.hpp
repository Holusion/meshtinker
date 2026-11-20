#pragma once

// Chains: a directory holding each command's result as a numbered stage
// (docs/chaining.md).

#include "meshtinker/common/cleanup.hpp"
#include "meshtinker/common/diagnostics.hpp"
#include "meshtinker/common/file_lock.hpp"
#include "meshtinker/common/outputs.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace meshtinker::chain {

using Json = nlohmann::ordered_json;

// The version of chain.json and stage.json that this build writes. Readers
// refuse a newer one and ignore fields they don't know.
constexpr int format = 1;

constexpr const char* chain_file = "chain.json";
constexpr const char* stage_file = "stage.json";
constexpr const char* lock_file = "lock";

// A stage directory's name, `NNN-<command>-<id>`.
struct StageName {
    std::uint64_t number = 0;
    std::string command;
    std::string id;

    // The directory name: the number on three digits or more.
    std::string text() const;
};

// Parses a stage directory's name: the number is the part before the first
// hyphen (three digits or more), the id the part after the last one (five
// characters from a-z0-9), the command what is between.
std::optional<StageName> parseStageName(const std::string& text);

// Five random characters from a-z0-9.
std::string randomId();

// Whether `path` is a directory holding chain.json.
bool isChain(const std::filesystem::path& path);

// The running total of lossy rounds that a stage records, copied from its
// parent and extended by the stage.
class Lossy {
public:
    // `parent` is the parent stage's `lossy` (null for stage 000), `stage`
    // the new stage's name.
    Lossy(Json parent, std::string stage);

    // Appends a round to the array `kind` (`geometryQuantization`,
    // `imageEncoding`), as `entry` with the stage's name in front. When an
    // earlier stage recorded a round of that kind already, and `diagnostics`
    // is given, warns: errors add up. The codes are `repeated-quantization`
    // and `repeated-lossy-image-encoding`, `repeated-<kind>` for others.
    void addRound(const std::string& kind, const Json& entry, common::Diagnostics* diagnostics);
    // Adds `value` to the number `key` (`simplificationError`).
    void addError(const std::string& key, double value);

    const Json& json() const { return json_; }

private:
    Json json_;
    std::string stage_;
    // The number of entries each array had in the parent.
    std::map<std::string, std::size_t> inherited_;
};

// An existing chain, read-only: chain.json and its published stages.
class Chain {
public:
    // Reads `directory`'s chain.json. Throws a Failure (exit code 1) if it
    // isn't a chain (`not-a-chain`), can't be read (`invalid-chain`), or has a
    // newer format (`chain-format-newer`).
    static Chain open(const std::filesystem::path& directory);

    const std::filesystem::path& directory() const { return directory_; }
    // The source file's name, as `import` read it.
    const std::string& source() const { return source_; }
    // The stem of every stage's model files: the source's.
    std::string stem() const;

    // The published stages, by number then name. A directory that doesn't
    // match `NNN-<command>-<id>` or has no stage.json is not one.
    std::vector<StageName> stages() const;
    // The stage with the highest number. Throws a Failure (exit code 1,
    // `chain-empty`) if there is none.
    StageName last() const;
    // `<stage>/<stem>.gltf`.
    std::filesystem::path model(const StageName& stage) const;
    // The stage's stage.json. Throws a Failure (exit code 1) if it can't be
    // read (`invalid-stage`) or has a newer format (`chain-format-newer`).
    Json record(const StageName& stage) const;

    // Takes the chain's lock, as a command writing a stage does. Throws a
    // Failure (exit code 4, `chain-in-use`) if another command holds it.
    common::FileLock lock() const;
    // Removes the chain, under its lock: chain.json first, so that it is no
    // longer a chain, then everything else in the directory, and the
    // directory. A directory that can't be removed (a mount point) stays
    // empty, with a warning (`chain-directory-kept`).
    void remove(const common::FileLock& lock, common::Diagnostics& diagnostics) const;

private:
    Chain(std::filesystem::path directory, std::string source) : directory_(std::move(directory)), source_(std::move(source)) {}

    std::filesystem::path directory_;
    std::string source_;
};

// A stage being written: the chain's lock, held until the object goes, and a
// pending directory in `outputs`, which commit() publishes. The command
// writes its model into directory() with `outputs`, then calls finish().
class StageWriter {
public:
    // Adds a stage built on the last one. Throws a Failure (exit code 4,
    // `chain-in-use`) if another command holds the lock. Files that the
    // stage writes identical to the parent's are linked (Outputs::linkFrom).
    static std::unique_ptr<StageWriter> add(const Chain& chain, const std::string& command, common::Outputs& outputs);

    // Creates a chain in `directory` for `source` (`import`), with stage 000.
    // `directory` must not exist, or be empty; its parent must exist (exit
    // code 4, `output-exists` or `output-directory-missing`). chain.json is
    // published after the stage, so the directory is a chain only once it
    // has one. If the command fails, what this created goes, the directory
    // included.
    static std::unique_ptr<StageWriter> create(const std::filesystem::path& directory, const std::string& source,
                                               const std::string& command, common::Outputs& outputs);

    StageWriter(const StageWriter&) = delete;
    StageWriter& operator=(const StageWriter&) = delete;
    StageWriter(StageWriter&&) = delete;
    StageWriter& operator=(StageWriter&&) = delete;
    ~StageWriter();

    const std::filesystem::path& chainDirectory() const { return chain_directory_; }
    // The pending stage directory, where the command writes its files.
    const std::filesystem::path& directory() const { return directory_; }
    // The stem of the model files.
    const std::string& stem() const { return stem_; }
    // The parent stage's model, the command's input. Empty for stage 000.
    const std::optional<std::filesystem::path>& parentModel() const { return parent_model_; }
    const StageName& name() const { return name_; }
    Lossy& lossy() { return *lossy_; }

    // Writes stage.json, with the command's JSON report, into the stage
    // (and chain.json for a new chain). `tool` is `meshtinker-<name>
    // <version>`. The caller then commits `outputs`, which publishes the
    // stage.
    void finish(const Json& report, const std::string& tool, const std::vector<std::string>& argv);
    // Marks the chain as kept, once `outputs` committed.
    void published() { published_ = true; }

private:
    StageWriter(common::Outputs& outputs, common::FileLock lock) : outputs_(outputs), lock_(std::move(lock)) {}

    common::Outputs& outputs_;
    common::FileLock lock_;
    std::filesystem::path chain_directory_;
    std::filesystem::path directory_;
    std::string stem_;
    std::string source_;
    std::optional<std::filesystem::path> parent_model_;
    std::optional<StageName> parent_;
    StageName name_;
    std::string command_;
    std::unique_ptr<Lossy> lossy_;
    // For a new chain: what to remove if the command fails.
    bool created_directory_ = false;
    bool new_chain_ = false;
    bool published_ = false;
    std::vector<common::CleanupRegistration> cleanup_;
};

} // namespace meshtinker::chain
