#include "meshtinker/common/references.hpp"

#include "meshtinker/common/failure.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <vector>

namespace meshtinker::common {
namespace fs = std::filesystem;

namespace {

fs::path nonEmpty(const fs::path& directory) { return directory.empty() ? fs::path(".") : directory; }

// `C:`, `http:` and the like: letters, then letters, digits, `+`, `-` or `.`,
// then a colon. A drive letter is the one-letter case of a scheme.
bool hasScheme(const std::string& written) {
    const std::size_t colon = written.find(':');
    if (colon == std::string::npos || colon == 0) {
        return false;
    }
    if (std::isalpha(static_cast<unsigned char>(written[0])) == 0) {
        return false;
    }
    return std::all_of(written.begin(), written.begin() + static_cast<std::ptrdiff_t>(colon), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '+' || c == '-' || c == '.';
    });
}

// A relative path: not empty, and no root, drive letter, URI scheme or NUL byte.
bool looksRelative(const std::string& written) {
    return !written.empty() && written.find('\0') == std::string::npos && written.front() != '/' &&
           !hasScheme(written);
}

// Whether the text reaches above the directory `depth` levels below the root.
// This reads the text only: symlinks are for regularFileInside to catch.
bool climbsOut(const std::string& written, std::size_t depth) {
    std::size_t start = 0;
    while (start <= written.size()) {
        std::size_t end = written.find('/', start);
        if (end == std::string::npos) {
            end = written.size();
        }
        const std::string part = written.substr(start, end - start);
        if (part == "..") {
            if (depth == 0) {
                return true;
            }
            --depth;
        } else if (!part.empty() && part != ".") {
            ++depth;
        }
        start = end + 1;
    }
    return false;
}

bool isInside(const fs::path& root, const fs::path& file) {
    auto root_part = root.begin();
    auto file_part = file.begin();
    for (; root_part != root.end(); ++root_part, ++file_part) {
        if (file_part == file.end() || *root_part != *file_part) {
            return false;
        }
    }
    return true;
}

// The kernel's view of the path, which is what catches `link-to-outside/../file`.
// The type check comes before any open, because opening a FIFO would block.
std::optional<fs::path> regularFile(const fs::path& candidate, const fs::path& root) {
    std::error_code error;
    fs::path file = fs::canonical(candidate, error);
    if (error || !fs::is_regular_file(file, error) || error) {
        return std::nullopt;
    }
    if (!root.empty() && !isInside(root, file)) {
        return std::nullopt;
    }
    return file;
}

} // namespace

References References::forInput(const fs::path& directory, bool trusted) {
    References references;
    if (!trusted) {
        std::error_code error;
        references.root_ = fs::canonical(nonEmpty(directory), error);
        if (error) {
            throw Failure(classifySystemError(error.value(), Side::input), "file-unreadable",
                          "cannot resolve the input's folder: " + error.message());
        }
    }
    return references;
}

std::optional<fs::path> References::resolve(const fs::path& base, const std::string& reference) const {
    std::string written = reference;
    std::replace(written.begin(), written.end(), '\\', '/');
    if (root_.empty()) {
        // Absolute paths are fine; URLs and drive letters can't be read here.
        if (written.empty() || written.find('\0') != std::string::npos || hasScheme(written)) {
            return std::nullopt;
        }
        return regularFile(nonEmpty(base) / fs::u8path(written), {});
    }
    // Decided from the text alone, so the path of a refused reference is never looked up.
    if (!looksRelative(written)) {
        return std::nullopt;
    }
    std::error_code error;
    const fs::path canonical_base = fs::canonical(nonEmpty(base), error);
    if (error || !isInside(root_, canonical_base)) {
        return std::nullopt;
    }
    std::size_t depth = 0;
    for (const fs::path& part : canonical_base.lexically_relative(root_)) {
        if (part != ".") {
            ++depth;
        }
    }
    if (climbsOut(written, depth)) {
        return std::nullopt;
    }
    return regularFile(canonical_base / fs::u8path(written), root_);
}

std::optional<std::string> References::locate(const std::string& reference) const {
    std::string written = reference;
    std::replace(written.begin(), written.end(), '\\', '/');
    if (root_.empty() ? (written.empty() || written.find('\0') != std::string::npos || hasScheme(written))
                      : (!looksRelative(written) || climbsOut(written, 0))) {
        return std::nullopt;
    }
    // Lexically: `..` cancels the step before it, and stays only where it
    // climbs above a relative start, which only a trusted reference can.
    const bool absolute = written.front() == '/';
    std::vector<std::string> steps;
    std::size_t start = 0;
    while (start <= written.size()) {
        std::size_t end = written.find('/', start);
        if (end == std::string::npos) {
            end = written.size();
        }
        std::string part = written.substr(start, end - start);
        if (part == "..") {
            if (!steps.empty() && steps.back() != "..") {
                steps.pop_back();
            } else if (!absolute) {
                steps.push_back(std::move(part));
            }
        } else if (!part.empty() && part != ".") {
            steps.push_back(std::move(part));
        }
        start = end + 1;
    }
    if (steps.empty()) {
        return std::nullopt;
    }
    std::string located = absolute ? "/" : "";
    for (std::size_t i = 0; i < steps.size(); ++i) {
        located += (i == 0 ? "" : "/") + steps[i];
    }
    return located;
}

} // namespace meshtinker::common
