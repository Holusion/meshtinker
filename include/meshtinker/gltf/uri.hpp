#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace meshtinker::gltf {

// Decodes %XX escapes. Malformed escapes are kept as written, like cgltf_decode_uri.
// Nothing else is special: `#` and `?` are part of the path, because exporters
// write file names unescaped.
std::string decodeUri(const std::string& uri);

// Escapes a file name for a `uri`: every byte except ASCII letters, digits and
// `-._~` becomes %XX (UTF-8 bytes for non-ASCII), so the reference reads the
// same in every URI resolver, including the URL class three.js uses.
std::string encodeUriSegment(const std::string& name);

bool isDataUri(const std::string& uri);

struct DataUri {
    std::string media_type;
    std::string base64;
};

// Splits `data:<media type>;base64,<data>`. Empty for anything else,
// including data URIs that aren't base64.
std::optional<DataUri> parseDataUri(const std::string& uri);

// Decodes standard base64, ignoring ASCII whitespace. Empty if the text isn't base64.
std::optional<std::vector<std::uint8_t>> decodeBase64(const std::string& text);

} // namespace meshtinker::gltf
