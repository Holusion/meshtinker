#pragma once

#include <iostream>
#include <sstream>
#include <string>

namespace meshtinker::test {

// What std::cerr receives while this lives, for checking log lines.
class StderrCapture {
public:
    StderrCapture() : old_(std::cerr.rdbuf(text_.rdbuf())) {}
    ~StderrCapture() { std::cerr.rdbuf(old_); }
    StderrCapture(const StderrCapture&) = delete;
    StderrCapture& operator=(const StderrCapture&) = delete;

    std::string text() const { return text_.str(); }

private:
    std::ostringstream text_;
    std::streambuf* old_;
};

} // namespace meshtinker::test
