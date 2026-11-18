#include "support/check.hpp"

#include <algorithm>
#include <exception>
#include <iostream>
#include <vector>

namespace meshtinker::test {
namespace {

struct Case {
    const char* name;
    CaseFunction function;
};

std::vector<Case>& cases() {
    static std::vector<Case> registered;
    return registered;
}

int failures_in_case = 0;

} // namespace

Registration::Registration(const char* name, CaseFunction function) { cases().push_back({name, function}); }

void fail(const std::string& message, const char* file, int line) {
    ++failures_in_case;
    std::cerr << file << ':' << line << ": check failed: " << message << '\n';
}

int failureCount() { return failures_in_case; }

} // namespace meshtinker::test

int main(int argc, char** argv) {
    using namespace meshtinker::test;
    const std::vector<std::string> selected(argv + 1, argv + argc);
    int failed = 0;
    int run = 0;
    for (const Case& test_case : cases()) {
        if (!selected.empty() && std::find(selected.begin(), selected.end(), test_case.name) == selected.end()) {
            continue;
        }
        ++run;
        failures_in_case = 0;
        try {
            test_case.function();
        } catch (const std::exception& error) {
            fail(std::string("uncaught exception: ") + error.what(), test_case.name, 0);
        }
        if (failures_in_case > 0) {
            ++failed;
            std::cerr << "FAILED " << test_case.name << '\n';
        }
    }
    if (run == 0) {
        std::cerr << "no test case matched\n";
        return 1;
    }
    std::cerr << run - failed << '/' << run << " cases passed\n";
    return failed == 0 ? 0 : 1;
}
