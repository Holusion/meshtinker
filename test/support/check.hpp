#pragma once

// A minimal test runner. Each test file defines cases with TEST_CASE and checks
// with CHECK; check.cpp provides main(). Run a test executable with case names
// as arguments to run only those cases.
//
//   TEST_CASE(parses_empty_input) {
//       CHECK(parse("").empty());
//       CHECK_EQ(parse("a").size(), 1u);
//   }

#include <sstream>
#include <string>

namespace meshtinker::test {

using CaseFunction = void (*)();

struct Registration {
    Registration(const char* name, CaseFunction function);
};

void fail(const std::string& message, const char* file, int line);

// Failed checks so far in the running case.
int failureCount();

template <typename A, typename B>
void checkEqual(const A& actual, const B& expected, const char* actual_text, const char* expected_text,
                const char* file, int line) {
    if (actual == expected) {
        return;
    }
    std::ostringstream message;
    message << actual_text << " == " << expected_text << "\n    actual:   " << actual
            << "\n    expected: " << expected;
    fail(message.str(), file, line);
}

} // namespace meshtinker::test

#define TEST_CASE(name)                                                                             \
    static void name();                                                                             \
    static const ::meshtinker::test::Registration name##_registration{#name, name};                 \
    static void name()

#define CHECK(condition)                                                                            \
    do {                                                                                            \
        if (!(condition)) {                                                                         \
            ::meshtinker::test::fail(#condition, __FILE__, __LINE__);                               \
        }                                                                                           \
    } while (false)

#define CHECK_EQ(actual, expected)                                                                  \
    ::meshtinker::test::checkEqual((actual), (expected), #actual, #expected, __FILE__, __LINE__)
