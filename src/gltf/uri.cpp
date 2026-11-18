#include "meshtinker/gltf/uri.hpp"

namespace meshtinker::gltf {
namespace {

int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

} // namespace

std::string decodeUri(const std::string& uri) {
    std::string decoded;
    decoded.reserve(uri.size());
    for (std::size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            const int high = hexValue(uri[i + 1]);
            const int low = hexValue(uri[i + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>(high * 16 + low));
                i += 2;
                continue;
            }
        }
        decoded.push_back(uri[i]);
    }
    return decoded;
}

std::string encodeUriSegment(const std::string& name) {
    static const char digits[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(name.size());
    for (const char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        const bool unreserved = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                                (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' || byte == '_' ||
                                byte == '~';
        if (unreserved) {
            encoded.push_back(c);
        } else {
            encoded.push_back('%');
            encoded.push_back(digits[byte >> 4]);
            encoded.push_back(digits[byte & 0x0F]);
        }
    }
    return encoded;
}

bool isDataUri(const std::string& uri) { return uri.compare(0, 5, "data:") == 0; }

std::optional<DataUri> parseDataUri(const std::string& uri) {
    if (!isDataUri(uri)) {
        return std::nullopt;
    }
    const std::size_t comma = uri.find(',');
    constexpr std::size_t marker_length = 7; // ";base64"
    if (comma == std::string::npos || comma < 5 + marker_length ||
        uri.compare(comma - marker_length, marker_length, ";base64") != 0) {
        return std::nullopt;
    }
    return DataUri{uri.substr(5, comma - marker_length - 5), uri.substr(comma + 1)};
}

std::optional<std::vector<std::uint8_t>> decodeBase64(const std::string& text) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(text.size() / 4 * 3);
    std::uint32_t accumulator = 0;
    int bits = 0;
    std::size_t padding = 0;
    for (const char c : text) {
        int value = -1;
        if (c >= 'A' && c <= 'Z') {
            value = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            value = c - 'a' + 26;
        } else if (c >= '0' && c <= '9') {
            value = c - '0' + 52;
        } else if (c == '+') {
            value = 62;
        } else if (c == '/') {
            value = 63;
        } else if (c == '=') {
            ++padding;
            continue;
        } else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            continue;
        } else {
            return std::nullopt;
        }
        if (padding > 0) {
            return std::nullopt; // data after padding
        }
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xFF));
        }
    }
    if (padding > 2) {
        return std::nullopt;
    }
    return bytes;
}

} // namespace meshtinker::gltf
