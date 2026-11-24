#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>

namespace meshtinker::common {

// Levels by increasing severity. The names are eCorpus's task log levels,
// without `fatal`: a command that fails logs an `error` and exits non-zero.
enum class LogLevel : std::uint8_t { trace, debug, info, warn, error };

// How messages are written on stderr (docs/logs.md): `text` for people, or
// `json`, one object per line, `{"level", "message", "code"}`.
enum class LogFormat : std::uint8_t { text, json };

const char* logLevelName(LogLevel level);
std::optional<LogLevel> parseLogLevel(const std::string& name);
std::optional<LogFormat> parseLogFormat(const std::string& name);

// The process's messages on stderr: everything but a command's data output.
// Messages below the threshold are dropped. Lines are written whole, from any
// thread.
class Log {
public:
    // `program` (`meshtinker-<name>`) starts every text line.
    void configure(std::string program, LogLevel threshold, LogFormat format);
    LogLevel threshold() const { return threshold_; }
    LogFormat format() const { return format_; }
    // Whether a message at `level` is written, to skip building one that isn't.
    bool enabled(LogLevel level) const { return level >= threshold_; }

    // One line. `code` is a diagnostic's or a failure's stable code, the same
    // as in JSON reports, and empty for other messages.
    void write(LogLevel level, const std::string& message, const std::string& code = "");
    void trace(const std::string& message) { write(LogLevel::trace, message); }
    void debug(const std::string& message) { write(LogLevel::debug, message); }
    void info(const std::string& message) { write(LogLevel::info, message); }
    void warn(const std::string& message, const std::string& code = "") { write(LogLevel::warn, message, code); }
    void error(const std::string& message, const std::string& code = "") { write(LogLevel::error, message, code); }

    // The line write() prints, without its newline.
    std::string line(LogLevel level, const std::string& message, const std::string& code) const;

private:
    std::string program_ = "meshtinker";
    LogLevel threshold_ = LogLevel::info;
    LogFormat format_ = LogFormat::text;
    std::mutex mutex_;
};

// The one Log of the process, which cli::Command configures from
// `--log-level` and `--log-format`.
Log& logger();

} // namespace meshtinker::common
