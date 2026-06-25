#include "config/fernet.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

using mirobody::encrypt::Fernet;
using mirobody::encrypt::FernetError;

namespace {

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

}

//------------------------------------------------------------------------------

TEST_CASE("generate_key produces a constructor-acceptable key", "[fernet]") {
    auto key = Fernet::generate_key();
    REQUIRE(key.size() == 44);
    REQUIRE_NOTHROW(Fernet{key});

    // Distinct calls should produce distinct keys (statistically certain).
    REQUIRE(key != Fernet::generate_key());
}

//------------------------------------------------------------------------------

TEST_CASE("encrypt/decrypt roundtrip across boundary sizes", "[fernet]") {
    Fernet f{Fernet::generate_key()};

    const std::string cases[] = {
        "",                       // empty
        "x",                      // 1 byte
        std::string(15, 'a'),     // one short of a block
        std::string(16, 'b'),     // exactly one block (forces a full padding block)
        std::string(17, 'c'),     // just over a block
        std::string(4096, 'd'),   // multi-page
    };
    for (const auto& pt : cases) {
        CAPTURE(pt.size());
        auto token = f.encrypt(pt);
        REQUIRE(f.decrypt(token) == pt);
    }
}

//------------------------------------------------------------------------------

TEST_CASE("each encryption uses a fresh IV", "[fernet]") {
    Fernet f{Fernet::generate_key()};
    auto a = f.encrypt("same plaintext");
    auto b = f.encrypt("same plaintext");
    REQUIRE(a != b);
    REQUIRE(f.decrypt(a) == "same plaintext");
    REQUIRE(f.decrypt(b) == "same plaintext");
}

//------------------------------------------------------------------------------

TEST_CASE("decrypt with a different key fails authentication", "[fernet]") {
    Fernet a{Fernet::generate_key()};
    Fernet b{Fernet::generate_key()};
    auto token = a.encrypt("secret");
    REQUIRE_THROWS_AS(b.decrypt(token), FernetError);
}

//------------------------------------------------------------------------------

TEST_CASE("tampered token fails HMAC", "[fernet]") {
    Fernet f{Fernet::generate_key()};
    auto token = f.encrypt("payload");
    REQUIRE(token.size() > 20);

    // Flip a byte in the middle (lands in the IV or ciphertext region;
    // HMAC must reject before any plaintext is returned).
    std::size_t idx = token.size() / 2;
    token[idx] = (token[idx] == 'A') ? 'B' : 'A';
    REQUIRE_THROWS_AS(f.decrypt(token), FernetError);
}

//------------------------------------------------------------------------------

TEST_CASE("malformed input is rejected cleanly", "[fernet]") {
    Fernet f{Fernet::generate_key()};

    SECTION("invalid base64") {
        REQUIRE_THROWS_AS(f.decrypt("!!!not base64!!!"), FernetError);
    }
    SECTION("too short") {
        REQUIRE_THROWS_AS(f.decrypt("aGVsbG8="), FernetError); // "hello"
    }
    SECTION("bad version byte") {
        // Build a syntactically large-enough buffer that decodes cleanly but
        // begins with version 0x00 rather than 0x80.
        auto token = f.encrypt("x");
        // Replace the first base64 char so the version byte becomes 0x00.
        // First char encodes the high 6 bits of byte 0; map 'g' (0x80 prefix)
        // to 'A' (0x00 prefix).
        REQUIRE(token[0] == 'g');
        token[0] = 'A';
        REQUIRE_THROWS_AS(f.decrypt(token), FernetError);
    }
}

//------------------------------------------------------------------------------

TEST_CASE("constructor rejects malformed keys", "[fernet]") {
    REQUIRE_THROWS_AS(Fernet{""}, FernetError);
    REQUIRE_THROWS_AS(Fernet{"not-a-real-key"}, FernetError);
    // Wrong decoded length (16 bytes instead of 32).
    REQUIRE_THROWS_AS(Fernet{"AAAAAAAAAAAAAAAAAAAAAA=="}, FernetError);
}

//------------------------------------------------------------------------------

TEST_CASE("TTL: old tokens are rejected, fresh tokens pass", "[fernet]") {
    Fernet f{Fernet::generate_key()};
    auto now = now_seconds();

    auto old_token = f.encrypt_at("payload", now - 3600);
    REQUIRE_THROWS_AS(f.decrypt(old_token, /*ttl_seconds*/ 60), FernetError);

    auto fresh = f.encrypt_at("payload", now);
    REQUIRE(f.decrypt(fresh, 60) == "payload");

    // ttl_seconds == 0 disables the age check entirely.
    REQUIRE(f.decrypt(old_token) == "payload");
    REQUIRE(f.decrypt(old_token, 0) == "payload");
}

//------------------------------------------------------------------------------

TEST_CASE("TTL: future-dated tokens tolerate up to 60s of skew", "[fernet]") {
    Fernet f{Fernet::generate_key()};
    auto now = now_seconds();

    auto barely_future = f.encrypt_at("payload", now + 30);
    REQUIRE(f.decrypt(barely_future, 60) == "payload");

    auto too_far_future = f.encrypt_at("payload", now + 120);
    REQUIRE_THROWS_AS(f.decrypt(too_far_future, 60), FernetError);
}
