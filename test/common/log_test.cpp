#include "meshtinker/common/log.hpp"
#include "support/check.hpp"

#include <nlohmann/json.hpp>

#include <iostream>
#include <sstream>
#include <string>

using namespace meshtinker::common;

namespace {

// What `write` prints on stderr.
std::string captured(Log& log, LogLevel level, const std::string& message, const std::string& code = "") {
    std::ostringstream err;
    std::streambuf* old = std::cerr.rdbuf(err.rdbuf());
    log.write(level, message, code);
    std::cerr.rdbuf(old);
    return err.str();
}

} // namespace

TEST_CASE(text_lines_follow_gnu_style) {
    Log log;
    log.configure("meshtinker-test", LogLevel::trace, LogFormat::text);
    CHECK_EQ(captured(log, LogLevel::info, "Running weld"), std::string("meshtinker-test: Running weld\n"));
    CHECK_EQ(captured(log, LogLevel::debug, "detail"), std::string("meshtinker-test: debug: detail\n"));
    CHECK_EQ(captured(log, LogLevel::trace, "detail"), std::string("meshtinker-test: trace: detail\n"));
    CHECK_EQ(captured(log, LogLevel::warn, "odd", "odd-thing"), std::string("meshtinker-test: warning: odd [odd-thing]\n"));
    CHECK_EQ(captured(log, LogLevel::error, "bad"), std::string("meshtinker-test: error: bad\n"));
}

TEST_CASE(json_lines_have_level_message_and_code) {
    Log log;
    log.configure("meshtinker-test", LogLevel::trace, LogFormat::json);
    CHECK_EQ(captured(log, LogLevel::info, "Running \"weld\""),
             std::string(R"({"level":"info","message":"Running \"weld\""})") + "\n");
    CHECK_EQ(captured(log, LogLevel::warn, "odd", "odd-thing"),
             std::string(R"({"level":"warn","message":"odd","code":"odd-thing"})") + "\n");
    // A file name that isn't UTF-8 is replaced, not thrown on.
    const auto line = nlohmann::json::parse(captured(log, LogLevel::error, std::string("bad \xff name")));
    CHECK_EQ(line.at("level").get<std::string>(), std::string("error"));
}

TEST_CASE(messages_below_the_threshold_are_dropped) {
    Log log;
    log.configure("meshtinker-test", LogLevel::warn, LogFormat::text);
    CHECK(captured(log, LogLevel::info, "quiet").empty());
    CHECK(captured(log, LogLevel::debug, "quiet").empty());
    CHECK(!captured(log, LogLevel::warn, "loud").empty());
    CHECK(!captured(log, LogLevel::error, "loud").empty());
    CHECK(!log.enabled(LogLevel::info));
}

TEST_CASE(level_and_format_names_parse) {
    for (const LogLevel level : {LogLevel::trace, LogLevel::debug, LogLevel::info, LogLevel::warn, LogLevel::error}) {
        CHECK(parseLogLevel(logLevelName(level)) == level);
    }
    CHECK(!parseLogLevel("warning"));
    CHECK(parseLogFormat("json") == LogFormat::json);
    CHECK(!parseLogFormat("yaml"));
}
