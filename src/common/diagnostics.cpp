#include "meshtinker/common/diagnostics.hpp"

#include "meshtinker/common/log.hpp"

#include <algorithm>

namespace meshtinker::common {

const char* severityName(Severity severity) { return severity == Severity::error ? "error" : "warning"; }

void Diagnostics::add(Severity severity, const std::string& code, const std::string& message, const std::string& example) {
    auto found = std::find_if(entries_.begin(), entries_.end(), [&](const Diagnostic& d) { return d.code == code; });
    if (found == entries_.end()) {
        entries_.push_back({code, severity, 0, message, {}});
        found = entries_.end() - 1;
    }
    ++found->count;
    if (severity == Severity::error) {
        found->severity = Severity::error;
    }
    if (!example.empty() && found->examples.size() < max_examples) {
        found->examples.push_back(example);
    }
}

nlohmann::ordered_json Diagnostics::toJson() const {
    nlohmann::ordered_json list = nlohmann::ordered_json::array();
    for (const Diagnostic& d : entries_) {
        list.push_back({{"code", d.code},
                        {"severity", severityName(d.severity)},
                        {"count", d.count},
                        {"message", d.message},
                        {"examples", d.examples}});
    }
    return list;
}

void Diagnostics::log() const {
    for (const Diagnostic& d : entries_) {
        std::string message = d.message;
        if (d.count > 1 || !d.examples.empty()) {
            message += " (" + std::to_string(d.count) + (d.count == 1 ? " time" : " times");
            for (std::size_t i = 0; i < d.examples.size(); ++i) {
                message += (i == 0 ? ", e.g. " : "; ") + d.examples[i];
            }
            message += ")";
        }
        logger().write(d.severity == Severity::error ? LogLevel::error : LogLevel::warn, message, d.code);
    }
}

} // namespace meshtinker::common
