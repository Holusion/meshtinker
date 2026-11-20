#include "meshtinker/chain/chain.hpp"

#include "meshtinker/common/failure.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <random>
#include <system_error>

namespace meshtinker::chain {
namespace fs = std::filesystem;
using common::ExitCode;
using common::Failure;

namespace {

Failure rejected(const std::string& code, const std::string& message) {
    return Failure(ExitCode::input_rejected, code, message);
}

std::string quoted(const fs::path& path) { return "'" + path.filename().u8string() + "'"; }

// Reads a small JSON file, such as chain.json or stage.json.
Json readJson(const fs::path& path, const std::string& code) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw rejected(code, "cannot read " + quoted(path));
    }
    Json json = Json::parse(stream, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        throw rejected(code, quoted(path) + " is not a JSON object");
    }
    const auto version = json.find("format");
    if (version == json.end() || !version->is_number_integer()) {
        throw rejected(code, quoted(path) + " has no format");
    }
    if (version->get<std::int64_t>() > format) {
        throw rejected("chain-format-newer", quoted(path) + " has format " + version->dump() +
                                                 ", newer than this version reads (" + std::to_string(format) + ")");
    }
    return json;
}

std::string utcNow() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm parts{};
#if defined(_WIN32)
    gmtime_s(&parts, &now);
#else
    gmtime_r(&now, &parts);
#endif
    char text[32];
    std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &parts);
    return text;
}

void writeJson(common::Outputs& outputs, const fs::path& path, const Json& json) {
    common::OutputFile file = outputs.create(path);
    file.write(json.dump(2) + "\n");
    file.close();
}

common::FileLock lockChain(const fs::path& directory) {
    std::optional<common::FileLock> lock = common::FileLock::tryAcquire(directory / lock_file);
    if (!lock) {
        throw Failure(ExitCode::machine, "chain-in-use",
                      "another command is writing a stage of " + quoted(directory) + "; try again once it has finished");
    }
    return std::move(*lock);
}

} // namespace

std::string StageName::text() const {
    char number_text[24];
    std::snprintf(number_text, sizeof number_text, "%03llu", static_cast<unsigned long long>(number));
    return std::string(number_text) + "-" + command + "-" + id;
}

std::optional<StageName> parseStageName(const std::string& text) {
    const std::size_t first = text.find('-');
    const std::size_t last = text.rfind('-');
    if (first == std::string::npos || last <= first + 1 || first < 3 || first > 18) {
        return std::nullopt;
    }
    const std::string number = text.substr(0, first);
    const std::string id = text.substr(last + 1);
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    const auto id_char = [&](char c) { return digit(c) || (c >= 'a' && c <= 'z'); };
    if (!std::all_of(number.begin(), number.end(), digit) || id.size() != 5 ||
        !std::all_of(id.begin(), id.end(), id_char)) {
        return std::nullopt;
    }
    return StageName{std::stoull(number), text.substr(first + 1, last - first - 1), id};
}

std::string randomId() {
    static constexpr char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::random_device device;
    std::string id;
    for (int i = 0; i < 5; ++i) {
        id.push_back(alphabet[device() % (sizeof alphabet - 1)]);
    }
    return id;
}

bool isChain(const fs::path& path) {
    std::error_code error;
    return fs::is_directory(path, error) && fs::is_regular_file(path / chain_file, error);
}

Lossy::Lossy(Json parent, std::string stage) : json_(std::move(parent)), stage_(std::move(stage)) {
    if (!json_.is_object()) {
        json_ = Json::object();
    }
    for (const auto& [key, value] : json_.items()) {
        if (value.is_array()) {
            inherited_[key] = value.size();
        }
    }
}

void Lossy::addRound(const std::string& kind, const Json& entry, common::Diagnostics* diagnostics) {
    Json& rounds = json_[kind];
    if (!rounds.is_array()) {
        rounds = Json::array();
    }
    const auto inherited = inherited_.find(kind);
    if (diagnostics != nullptr && inherited != inherited_.end() && inherited->second > 0) {
        if (kind == "geometryQuantization") {
            diagnostics->warn("repeated-quantization",
                              "geometry is quantized again: an earlier stage quantized it already, and errors add up",
                              rounds[0].value("stage", std::string()));
        } else if (kind == "imageEncoding") {
            diagnostics->warn("repeated-lossy-image-encoding",
                              "images are encoded lossily again: an earlier stage did already, and errors add up",
                              rounds[0].value("stage", std::string()));
        } else {
            diagnostics->warn("repeated-" + kind, "a lossy round of the same kind is in an earlier stage already",
                              rounds[0].value("stage", std::string()));
        }
    }
    Json round = Json{{"stage", stage_}};
    for (const auto& [key, value] : entry.items()) {
        round[key] = value;
    }
    rounds.push_back(std::move(round));
}

void Lossy::addError(const std::string& key, double value) {
    Json& total = json_[key];
    total = (total.is_number() ? total.get<double>() : 0.0) + value;
}

Chain Chain::open(const fs::path& directory) {
    if (!isChain(directory)) {
        throw rejected("not-a-chain", quoted(directory) + " is not a chain: it has no chain.json");
    }
    const Json json = readJson(directory / chain_file, "invalid-chain");
    const auto source = json.find("source");
    if (source == json.end() || !source->is_string() || source->get<std::string>().empty()) {
        throw rejected("invalid-chain", "chain.json has no source");
    }
    return Chain(directory, source->get<std::string>());
}

std::string Chain::stem() const { return fs::u8path(source_).stem().u8string(); }

std::vector<StageName> Chain::stages() const {
    std::vector<StageName> stages;
    std::error_code error;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory_, error)) {
        const std::optional<StageName> name = parseStageName(entry.path().filename().u8string());
        std::error_code ignored;
        if (name && entry.is_directory(ignored) && fs::is_regular_file(entry.path() / stage_file, ignored)) {
            stages.push_back(*name);
        }
    }
    if (error) {
        throw common::systemFailure(error.value(), common::Side::input, "invalid-chain",
                                    "cannot list " + quoted(directory_));
    }
    std::sort(stages.begin(), stages.end(), [](const StageName& a, const StageName& b) {
        return a.number != b.number ? a.number < b.number : a.text() < b.text();
    });
    return stages;
}

StageName Chain::last() const {
    const std::vector<StageName> all = stages();
    if (all.empty()) {
        throw rejected("chain-empty", quoted(directory_) + " has no stage");
    }
    return all.back();
}

fs::path Chain::model(const StageName& stage) const { return directory_ / fs::u8path(stage.text()) / fs::u8path(stem() + ".gltf"); }

Json Chain::record(const StageName& stage) const {
    return readJson(directory_ / fs::u8path(stage.text()) / stage_file, "invalid-stage");
}

std::unique_ptr<StageWriter> StageWriter::add(const Chain& chain, const std::string& command, common::Outputs& outputs) {
    common::FileLock lock = lockChain(chain.directory());
    // Read under the lock, so no other stage can take the next number.
    const StageName parent = chain.last();
    const Json record = chain.record(parent);
    std::unique_ptr<StageWriter> writer(new StageWriter(outputs, std::move(lock)));
    writer->chain_directory_ = chain.directory();
    writer->stem_ = chain.stem();
    writer->source_ = chain.source();
    writer->parent_ = parent;
    writer->parent_model_ = chain.model(parent);
    writer->command_ = command;
    writer->name_ = StageName{parent.number + 1, command, randomId()};
    const auto lossy = record.find("lossy");
    writer->lossy_ = std::make_unique<Lossy>(lossy == record.end() ? Json() : *lossy, writer->name_.text());
    writer->directory_ = outputs.createDirectory(chain.directory() / fs::u8path(writer->name_.text()));
    outputs.linkFrom(chain.directory() / fs::u8path(parent.text()));
    return writer;
}

std::unique_ptr<StageWriter> StageWriter::create(const fs::path& directory, const std::string& source,
                                                 const std::string& command, common::Outputs& outputs) {
    std::error_code error;
    bool created = false;
    if (fs::exists(fs::symlink_status(directory, error))) {
        if (!fs::is_directory(directory, error) || !fs::is_empty(directory, error)) {
            throw Failure(ExitCode::machine, "output-exists",
                          quoted(directory) + " already exists" +
                              (fs::is_directory(directory, error) ? " and isn't empty" : ""));
        }
    } else {
        fs::path parent = directory.parent_path();
        if (parent.empty()) {
            parent = ".";
        }
        if (!fs::is_directory(parent, error)) {
            throw Failure(ExitCode::machine, "output-directory-missing", "the output's directory doesn't exist");
        }
        if (!fs::create_directory(directory, error)) {
            throw common::systemFailure(error ? error.value() : EEXIST, common::Side::machine, "output-failed",
                                        "cannot create " + quoted(directory));
        }
        created = true;
    }
    std::vector<common::CleanupRegistration> cleanup;
    if (created) {
        cleanup.emplace_back(directory.string());
    }
    std::optional<common::FileLock> lock;
    try {
        lock.emplace(lockChain(directory));
    } catch (...) {
        if (created) {
            fs::remove(directory, error);
        }
        throw;
    }
    if (lock->created()) {
        cleanup.emplace_back((directory / lock_file).string());
    }
    std::unique_ptr<StageWriter> writer(new StageWriter(outputs, std::move(*lock)));
    writer->cleanup_ = std::move(cleanup);
    writer->created_directory_ = created;
    writer->new_chain_ = true;
    writer->chain_directory_ = directory;
    writer->source_ = fs::u8path(source).filename().u8string();
    writer->stem_ = fs::u8path(writer->source_).stem().u8string();
    writer->command_ = command;
    writer->name_ = StageName{0, command, randomId()};
    writer->lossy_ = std::make_unique<Lossy>(Json(), writer->name_.text());
    writer->directory_ = outputs.createDirectory(directory / fs::u8path(writer->name_.text()));
    return writer;
}

StageWriter::~StageWriter() {
    if (!new_chain_ || published_) {
        return;
    }
    // A directory this run created holds only what the run wrote: the chain
    // isn't published, so no other command uses it. In a directory that was
    // there, the stage and chain.json are the Outputs' to remove.
    std::error_code ignored;
    if (created_directory_) {
        fs::remove_all(chain_directory_, ignored);
    } else if (lock_.created()) {
        fs::remove(chain_directory_ / lock_file, ignored);
    }
}

void StageWriter::finish(const Json& report, const std::string& tool, const std::vector<std::string>& argv) {
    Json record = Json::object();
    record["format"] = format;
    record["stage"] = name_.text();
    record["parent"] = parent_ ? Json(parent_->text()) : Json();
    record["command"] = command_;
    record["tool"] = tool;
    record["argv"] = argv;
    record["report"] = report;
    record["lossy"] = lossy_->json();
    writeJson(outputs_, directory_ / stage_file, record);
    if (new_chain_) {
        writeJson(outputs_, chain_directory_ / chain_file,
                  Json{{"format", format}, {"source", source_}, {"created", utcNow()}});
    }
}

} // namespace meshtinker::chain
