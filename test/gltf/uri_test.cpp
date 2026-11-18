#include "meshtinker/gltf/uri.hpp"
#include "support/check.hpp"

using namespace meshtinker::gltf;

TEST_CASE(decodes_percent_escapes) {
    CHECK_EQ(decodeUri("geo%20metry%20%E2%9D%A4.bin"), std::string("geo metry \xE2\x9D\xA4.bin"));
    CHECK_EQ(decodeUri("100%25%20%231.png"), std::string("100% #1.png"));
    // Unescaped names, as exporters write them, stay as they are.
    CHECK_EQ(decodeUri("a b#c?.png"), std::string("a b#c?.png"));
    // Malformed escapes are kept.
    CHECK_EQ(decodeUri("50%.png"), std::string("50%.png"));
    CHECK_EQ(decodeUri("%zz%2"), std::string("%zz%2"));
}

TEST_CASE(encodes_everything_but_unreserved_characters) {
    CHECK_EQ(encodeUriSegment("model_1-a.b~c.bin"), std::string("model_1-a.b~c.bin"));
    CHECK_EQ(encodeUriSegment("100% #1.png"), std::string("100%25%20%231.png"));
    CHECK_EQ(encodeUriSegment("\xE2\x99\xBB/x"), std::string("%E2%99%BB%2Fx"));
    CHECK_EQ(decodeUri(encodeUriSegment("any \xE2\x9D\xA4 %name#?.png")), std::string("any \xE2\x9D\xA4 %name#?.png"));
}

TEST_CASE(parses_base64_data_uris_only) {
    const auto parsed = parseDataUri("data:application/octet-stream;base64,AAEC");
    CHECK(parsed.has_value());
    if (parsed) {
        CHECK_EQ(parsed->media_type, std::string("application/octet-stream"));
        CHECK_EQ(parsed->base64, std::string("AAEC"));
    }
    CHECK(!parseDataUri("data:,plain").has_value());
    CHECK(!parseDataUri("model.bin").has_value());
    CHECK(isDataUri("data:image/png;base64,"));
}

TEST_CASE(decodes_base64) {
    const auto bytes = decodeBase64("AAEC/w==");
    CHECK(bytes.has_value());
    if (bytes) {
        CHECK_EQ(bytes->size(), 4u);
        CHECK(bytes->size() == 4 && (*bytes)[0] == 0 && (*bytes)[1] == 1 && (*bytes)[2] == 2 && (*bytes)[3] == 255);
    }
    CHECK_EQ(decodeBase64("SGVs\nbG8=").value_or(std::vector<std::uint8_t>()).size(), 5u);
    CHECK(!decodeBase64("AA*A").has_value());
    CHECK(!decodeBase64("AA==AA").has_value());
}
