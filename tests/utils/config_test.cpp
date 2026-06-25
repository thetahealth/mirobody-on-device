#include "config/store.hpp"
#include "config/fernet.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>

using mirobody::utils::ConfigStore;
using mirobody::encrypt::Fernet;
using mirobody::utils::LocalYamlStore;
using mirobody::utils::RemoteYamlStore;

namespace {

#ifdef _WIN32
void set_env(const char* key, const char* value) { _putenv_s(key, value); }
void unset_env(const char* key)                  { _putenv_s(key, ""); }
#else
void set_env(const char* key, const char* value) { setenv(key, value, 1); }
void unset_env(const char* key)                  { unsetenv(key); }
#endif

}

//------------------------------------------------------------------------------

TEST_CASE("derive_fernet_key produces a usable Fernet key", "[config]") {
    auto k1 = ConfigStore::derive_fernet_key("mysecret");
    auto k2 = ConfigStore::derive_fernet_key("mysecret");

    REQUIRE(k1 == k2);
    REQUIRE(k1.size() == 44);
    REQUIRE_NOTHROW(Fernet{k1});

    REQUIRE(ConfigStore::derive_fernet_key("a") != ConfigStore::derive_fernet_key("b"));

    // Inputs longer than 32 chars truncate to the first 32, so equal prefixes
    // derive equal keys regardless of trailing content.
    std::string base = "01234567890123456789012345678901"; // 32 chars
    REQUIRE(ConfigStore::derive_fernet_key(base) == ConfigStore::derive_fernet_key(base + "ignored"));
}

//------------------------------------------------------------------------------

TEST_CASE("encrypted YAML values are decrypted on load", "[config]") {
    // Use a key name unlikely to clash with the developer's shell environment,
    // since real env vars would override loaded values (which is by design).
    unset_env("MIROBODY_TEST_SECRET");
    unset_env("MIROBODY_TEST_LEVEL");

    auto key = Fernet::generate_key();
    auto enc = std::make_shared<Fernet>(key);

    auto token = enc->encrypt("sk-the-real-secret");
    std::string yaml = "MIROBODY_TEST_SECRET: " + token + "\n"
                       "MIROBODY_TEST_LEVEL: debug\n";

    LocalYamlStore cfg{"", enc};
    REQUIRE(cfg.load_yaml_string(yaml));

    REQUIRE(cfg.get_str("MIROBODY_TEST_SECRET") == "sk-the-real-secret");
    // Lookup is case-insensitive: lowercase key resolves to upper-cased entry.
    REQUIRE(cfg.get_str("mirobody_test_level") == "debug");
}

//------------------------------------------------------------------------------

TEST_CASE("plain (non-encrypted) values pass through unchanged", "[config]") {
    LocalYamlStore cfg;
    REQUIRE(cfg.load_yaml_string("HTTP_HOST: 0.0.0.0\nHTTP_PORT: 8080\n"));

    REQUIRE(cfg.get_str("HTTP_HOST") == "0.0.0.0");
    REQUIRE(cfg.get_int("HTTP_PORT") == 8080);
}

//------------------------------------------------------------------------------

TEST_CASE("environment variables override loaded values", "[config]") {
    LocalYamlStore cfg;
    REQUIRE(cfg.load_yaml_string("HTTP_HOST: 0.0.0.0\n"));

    set_env("HTTP_HOST", "127.0.0.1");
    REQUIRE(cfg.get_str("HTTP_HOST") == "127.0.0.1");
    unset_env("HTTP_HOST");
    REQUIRE(cfg.get_str("HTTP_HOST") == "0.0.0.0");
}

//------------------------------------------------------------------------------

TEST_CASE("missing keys return the supplied default", "[config]") {
    LocalYamlStore cfg;
    REQUIRE(cfg.get_str("NONEXISTENT", "fallback") == "fallback");
    REQUIRE(cfg.get_int("NONEXISTENT", 42) == 42);
    REQUIRE(cfg.get_bool("NONEXISTENT", true) == true);
}

//------------------------------------------------------------------------------

TEST_CASE("empty values count as unset and return the supplied default", "[config]") {
    unset_env("MIROBODY_TEST_EMPTY");

    LocalYamlStore cfg;
    REQUIRE(cfg.load_yaml_string("MIROBODY_TEST_EMPTY: ''\n"));
    REQUIRE(cfg.get_str("MIROBODY_TEST_EMPTY", "fallback") == "fallback");

    // Same for an env var set to the empty string (getenv_opt skips it).
    set_env("MIROBODY_TEST_EMPTY", "");
    REQUIRE(cfg.get_str("MIROBODY_TEST_EMPTY", "fallback") == "fallback");
    unset_env("MIROBODY_TEST_EMPTY");
}

//------------------------------------------------------------------------------

TEST_CASE("get_bool accepts YAML booleans and string forms", "[config]") {
    LocalYamlStore cfg;
    REQUIRE(cfg.load_yaml_string(
        "FLAG_A: true\n"
        "FLAG_B: false\n"
        "FLAG_C: \"True\"\n"
        "FLAG_D: 0\n"
        "FLAG_E: 1\n"
    ));

    REQUIRE(cfg.get_bool("FLAG_A") == true);
    REQUIRE(cfg.get_bool("FLAG_B") == false);
    REQUIRE(cfg.get_bool("FLAG_C") == true);
    REQUIRE(cfg.get_bool("FLAG_D") == false);
    REQUIRE(cfg.get_bool("FLAG_E") == true);
}

//------------------------------------------------------------------------------

TEST_CASE("get_dict and get_list read YAML containers", "[config]") {
    LocalYamlStore cfg;
    REQUIRE(cfg.load_yaml_string(
        "HEADERS:\n"
        "  X-One: alpha\n"
        "  X-Two: beta\n"
        "TOOLS:\n"
        "  - search\n"
        "  - fetch\n"
    ));

    auto d = cfg.get_dict("HEADERS");
    REQUIRE(d.size() == 2);
    REQUIRE(d.at("X-One") == "alpha");
    REQUIRE(d.at("X-Two") == "beta");

    auto l = cfg.get_list("TOOLS");
    REQUIRE(l == std::vector<std::string>{"search", "fetch"});
}

//------------------------------------------------------------------------------

TEST_CASE("get_dict and get_list also accept JSON-in-string scalars", "[config]") {
    LocalYamlStore cfg;
    REQUIRE(cfg.load_yaml_string(
        "HEADERS: '{\"X-A\": \"1\", \"X-B\": \"2\"}'\n"
        "TOOLS: '[\"a\", \"b\", \"c\"]'\n"
    ));

    auto d = cfg.get_dict("HEADERS");
    REQUIRE(d.size() == 2);
    REQUIRE(d.at("X-A") == "1");
    REQUIRE(d.at("X-B") == "2");

    auto l = cfg.get_list("TOOLS");
    REQUIRE(l == std::vector<std::string>{"a", "b", "c"});
}

//------------------------------------------------------------------------------

TEST_CASE("LocalYamlStore::load reads from disk", "[config]") {
    const char* tmp = std::getenv("TMP");
    if (!tmp) tmp = std::getenv("TEMP");
    if (!tmp) tmp = std::getenv("TMPDIR");
    if (!tmp) tmp = "/tmp";
    std::string path = std::string(tmp) + "/mirobody_config_test.yaml";
    {
        std::ofstream f(path);
        f << "MIROBODY_LOG_LEVEL: info\nMIROBODY_LISTEN_PORT: 9999\n";
    }

    LocalYamlStore cfg(path);
    REQUIRE(cfg.load());
    REQUIRE(cfg.get_str("MIROBODY_LOG_LEVEL") == "info");
    REQUIRE(cfg.get_int("MIROBODY_LISTEN_PORT") == 9999);

    std::remove(path.c_str());
}

//------------------------------------------------------------------------------

TEST_CASE("subsequent loads merge with last-wins semantics", "[config]") {
    LocalYamlStore cfg;
    REQUIRE(cfg.load_yaml_string("X: first\nY: 1\n"));
    REQUIRE(cfg.load_yaml_string("X: second\n"));

    REQUIRE(cfg.get_str("X") == "second");
    REQUIRE(cfg.get_int("Y") == 1);
}

//------------------------------------------------------------------------------

TEST_CASE("absorb layers another store underneath this one", "[config]") {
    LocalYamlStore lower;
    REQUIRE(lower.load_yaml_string("A: from_lower\nB: from_lower\n"));

    LocalYamlStore upper;
    REQUIRE(upper.load_yaml_string("B: from_upper\nC: from_upper\n"));

    // absorb copies every entry (overwriting), so caller orders calls to
    // pick the winner: pull the low-priority store in first, then load the
    // high-priority one last.
    LocalYamlStore merged;
    merged.absorb(lower);
    merged.absorb(upper);

    REQUIRE(merged.get_str("A") == "from_lower");
    REQUIRE(merged.get_str("B") == "from_upper");
    REQUIRE(merged.get_str("C") == "from_upper");
}

//------------------------------------------------------------------------------

TEST_CASE("RemoteYamlStore::load rejects empty server/token/env", "[config]") {
    REQUIRE_FALSE(RemoteYamlStore("",        "tok", "dev").load());
    REQUIRE_FALSE(RemoteYamlStore("http://x", "",   "dev").load());
    REQUIRE_FALSE(RemoteYamlStore("http://x", "tok", "").load());
}
