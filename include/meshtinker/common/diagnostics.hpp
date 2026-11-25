#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace meshtinker::common {

enum class Severity : std::uint8_t { warning, error };

// One kind of problem, counted: messy files give one line per kind, with up to
// three examples, instead of one line per occurrence.
struct Diagnostic {
    std::string code;
    Severity severity = Severity::warning;
    std::uint64_t count = 0;
    std::string message;
    std::vector<std::string> examples;
};

// The diagnostics a command gathers, in the order their codes first appear.
class Diagnostics {
public:
    static constexpr std::size_t max_examples = 3;

    // Counts one occurrence of `code`. The first message given for a code is
    // kept. `example` (such as `mesh "Rock", primitive 0`) is kept if fewer
    // than three are, and ignored when empty.
    void add(Severity severity, const std::string& code, const std::string& message, const std::string& example = "");
    void warn(const std::string& code, const std::string& message, const std::string& example = "") {
        add(Severity::warning, code, message, example);
    }

    const std::vector<Diagnostic>& entries() const { return entries_; }
    bool empty() const { return entries_.empty(); }

    // The `diagnostics` array of JSON reports (docs/inspect-report.md).
    nlohmann::ordered_json toJson() const;
    // One message each through common::logger(), at `warn` or `error`, with
    // the diagnostic's code: `<message> (<count> times, e.g. <examples>)`.
    void log() const;

private:
    std::vector<Diagnostic> entries_;
};

const char* severityName(Severity severity);

} // namespace meshtinker::common
