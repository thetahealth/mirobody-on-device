#include "storage/sign.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace mirobody::storage;

//------------------------------------------------------------------------------

TEST_CASE("sha256_hex matches FIPS 180-2 vectors", "[storage][sign]") {
    REQUIRE(sha256_hex("") ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    REQUIRE(sha256_hex("abc") ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

//------------------------------------------------------------------------------

TEST_CASE("base64_encode handles every padding length", "[storage][sign]") {
    REQUIRE(base64_encode("M")   == "TQ==");
    REQUIRE(base64_encode("Ma")  == "TWE=");
    REQUIRE(base64_encode("Man") == "TWFu");
    REQUIRE(base64_encode("any carnal pleasure.") == "YW55IGNhcm5hbCBwbGVhc3VyZS4=");
    REQUIRE(base64_encode("") == "");
}

//------------------------------------------------------------------------------

TEST_CASE("base64url_encode is unpadded and url-safe", "[storage][sign]") {
    // ">>>?" is bytes 0x3e 0x3e 0x3e 0x3f, which standard base64 renders with
    // '+' and '/' ("Pj4+Pw==") -- the url-safe form swaps them and drops '='.
    REQUIRE(base64url_encode(">>>?") == "Pj4-Pw");
    REQUIRE(base64url_encode("M")    == "TQ");
    REQUIRE(base64url_encode("Ma")   == "TWE");
    REQUIRE(base64url_encode("Man")  == "TWFu");
    REQUIRE(base64url_encode("")     == "");
}

//------------------------------------------------------------------------------

TEST_CASE("hmac_sha256 matches RFC 4231 test case 2", "[storage][sign]") {
    // key = "Jefe", data = "what do ya want for nothing?"
    const auto mac = hmac_sha256("Jefe", "what do ya want for nothing?");
    REQUIRE(hex_encode(mac.data(), mac.size()) ==
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

//------------------------------------------------------------------------------

TEST_CASE("uri_encode follows RFC 3986", "[storage][sign]") {
    REQUIRE(uri_encode("AZaz09-_.~", true) == "AZaz09-_.~");   // unreserved untouched
    REQUIRE(uri_encode("a b", true)        == "a%20b");
    REQUIRE(uri_encode("a/b", false)       == "a/b");          // slash kept
    REQUIRE(uri_encode("a/b", true)        == "a%2Fb");        // slash encoded
    REQUIRE(uri_encode("+=&", true)        == "%2B%3D%26");
}

//------------------------------------------------------------------------------

TEST_CASE("aws_sigv4_signature matches the AWS SigV4 'get-vanilla' test vector", "[storage][sign]") {
    // From the official aws-sig-v4-test-suite (get-vanilla): credentials
    // AKIDEXAMPLE / wJalrX...EXAMPLEKEY, region us-east-1, service "service",
    // date 20150830T123600Z. Building the string-to-sign from the documented
    // canonical request (hashed with our own, separately verified, sha256_hex)
    // exercises the full sign path end to end.
    const std::string secret = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
    const std::string canonical_request =
        "GET\n"
        "/\n"
        "\n"
        "host:example.amazonaws.com\n"
        "x-amz-date:20150830T123600Z\n"
        "\n"
        "host;x-amz-date\n"
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    REQUIRE(sha256_hex(canonical_request) ==
            "bb579772317eb040ac9ed261061d46c1f17a8133879d6129b6e1c25292927e63");

    const std::string string_to_sign =
        "AWS4-HMAC-SHA256\n"
        "20150830T123600Z\n"
        "20150830/us-east-1/service/aws4_request\n" + sha256_hex(canonical_request);

    const std::string sig =
        aws_sigv4_signature(secret, "20150830", "us-east-1", "service", string_to_sign);
    REQUIRE(sig == "ea21d6f05e96a897f6000a1a293f0a5bf0f92a00343409e820dce329ca6365ea");
}

//------------------------------------------------------------------------------

TEST_CASE("xml_tag_values extracts list-response keys", "[storage][sign]") {
    const std::string xml =
        "<?xml version=\"1.0\"?><ListBucketResult>"
        "<IsTruncated>true</IsTruncated>"
        "<Contents><Key>a/b.pdf</Key><Size>1</Size></Contents>"
        "<Contents><Key>a/c &amp; d.png</Key></Contents>"
        "<NextContinuationToken>tok==</NextContinuationToken>"
        "</ListBucketResult>";

    const std::vector<std::string> keys = mirobody::storage::xml_tag_values(xml, "Key");
    REQUIRE(keys.size() == 2);
    REQUIRE(keys[0] == "a/b.pdf");
    REQUIRE(keys[1] == "a/c & d.png");   // entity decoded

    REQUIRE(mirobody::storage::xml_tag_value(xml, "IsTruncated") == "true");
    REQUIRE(mirobody::storage::xml_tag_value(xml, "NextContinuationToken") == "tok==");
    REQUIRE(mirobody::storage::xml_tag_value(xml, "Missing").empty());
}
