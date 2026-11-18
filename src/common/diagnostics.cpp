#include "meshtinker/common/diagnostics.hpp"

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

void Diagnostics::print(std::ostream& out, const std::string& program) const {
    for (const Diagnostic& d : entries_) {
        out << program << ": " << severityName(d.severity) << ": " << d.message << " [" << d.code << "]";
        if (d.count > 1 || !d.examples.empty()) {
            out << " (" << d.count << (d.count == 1 ? " time" : " times");
            for (std::size_t i = 0; i < d.examples.size(); ++i) {
                out << (i == 0 ? ", e.g. " : "; ") << d.examples[i];
            }
            out << ")";
        }
        out << '\n';
    }
}

} // namespace meshtinker::common
