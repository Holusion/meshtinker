#include "meshtinker/common/log.hpp"

#include <nlohmann/json.hpp>

#include <iostream>
#include <utility>

namespace meshtinker::common {

namespace {

constexpr std::pair<const char*, LogLevel> level_names[] = {{"trace", LogLevel::trace},
                                                            {"debug", LogLevel::debug},
                                                            {"info", LogLevel::info},
                                                            {"warn", LogLevel::warn},
                                                            {"error", LogLevel::error}};

} // namespace

const char* logLevelName(LogLevel level) {
    for (const auto& [name, value] : level_names) {
        if (value == level) {
            return name;
        }
    }
    return "info";
}

std::optional<LogLevel> parseLogLevel(const std::string& name) {
    for (const auto& [text, value] : level_names) {
        if (name == text) {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<LogFormat> parseLogFormat(const std::string& name) {
    if (name == "text") {
        return LogFormat::text;
    }
    if (name == "json") {
        return LogFormat::json;
    }
    return std::nullopt;
}

void Log::configure(std::string program, LogLevel threshold, LogFormat format) {
    const std::lock_guard<std::mutex> lock(mutex_);
    program_ = std::move(program);
    threshold_ = threshold;
    format_ = format;
}

std::string Log::line(LogLevel level, const std::string& message, const std::string& code) const {
    if (format_ == LogFormat::json) {
        nlohmann::ordered_json json = {{"level", logLevelName(level)}, {"message", message}};
        if (!code.empty()) {
            json["code"] = code;
        }
        // File names in messages may not be UTF-8: replace, rather than throw.
        return json.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
    }
    // GNU style: `program: message` for information, with the severity
    // before the message otherwise.
    std::string text = program_ + ": ";
    switch (level) {
    case LogLevel::trace:
    case LogLevel::debug:
    case LogLevel::error:
        text += logLevelName(level);
        text += ": ";
        break;
    case LogLevel::warn:
        text += "warning: ";
        break;
    case LogLevel::info:
        break;
    }
    text += message;
    if (!code.empty()) {
        text += " [" + code + "]";
    }
    return text;
}

void Log::write(LogLevel level, const std::string& message, const std::string& code) {
    if (!enabled(level)) {
        return;
    }
    const std::string text = line(level, message, code) + '\n';
    const std::lock_guard<std::mutex> lock(mutex_);
    std::cerr << text << std::flush;
}

Log& logger() {
    static Log log;
    return log;
}

} // namespace meshtinker::common
