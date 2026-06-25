// fhir — debug CLI for the src/fhir module.
//
// Subcommands:
//   normalize <text>...     Unit normalization (Phase 1). For each input prints
//                           one JSON line {input, comparator, value, unit, family}.
//                           Pure local — no DB, no config. Mirrors the Python
//                           `python -m mirobody.indicator normalize`.
//   token <user_id>         Mint a bearer JWT for `user_id` using the configured
//                           JWT_KEY / JWT_SALT, for hitting the authenticated
//                           /fhir routes during local testing.
//
// Options:
//   --config <path>         YAML config to load (token subcommand). Falls back to
//                           MIROBODY_CONFIG, then ./config.yml.
//
// Examples:
//   fhir normalize "90次每分钟" "<5.6 mg/dL" "mmol/L"
//   fhir token 1 --config config.local.yaml

#include "config/config.hpp"
#include "compat/cxx11.hpp"
#include "fhir/units/families.hpp"
#include "fhir/units/normalize.hpp"
#include "jwt/jwt.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32")
#endif

namespace {

#ifdef _WIN32
// On Windows, main()'s char** argv arrives in the system ANSI code page, which
// mangles non-Latin terms (CJK / Cyrillic) before the units engine sees them.
// Re-derive the real arguments from the wide command line and re-encode UTF-8.
std::vector<std::string> utf8_args() {
    std::vector<std::string> out;
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!w) return out;
    for (int i = 0; i < n; ++i) {
        int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s;
        if (len > 1) {
            s.resize(static_cast<size_t>(len - 1));
            WideCharToMultiByte(CP_UTF8, 0, w[i], -1, &s[0], len, nullptr, nullptr);
        }
        out.push_back(s);
    }
    LocalFree(w);
    return out;
}
#endif

void print_usage(const char* argv0) {
    std::fprintf(stderr,
        "Usage:\n"
        "  %s normalize <text>...            Parse free-text value+unit -> UCUM + LOINC family\n"
        "  %s token <user_id> [--config P]   Mint a bearer JWT for the /fhir routes\n",
        argv0, argv0);
}

int cmd_normalize(const std::vector<std::string>& terms) {
    using namespace mirobody::fhir::units;
    if (terms.empty()) {
        std::fprintf(stderr, "normalize: no terms given\n");
        return 2;
    }
    for (size_t i = 0; i < terms.size(); ++i) {
        ParsedQuantity q = parse_value_unit(terms[i]);
        std::string fam = q.unit.empty() ? std::string() : unit_family(q.unit);

        rapidjson::Document d;
        d.SetObject();
        rapidjson::Document::AllocatorType& a = d.GetAllocator();
        d.AddMember("input", rapidjson::Value(terms[i].c_str(), a), a);
        d.AddMember("comparator", rapidjson::Value(q.comparator.c_str(), a), a);
        if (q.has_value) d.AddMember("value", q.value, a);
        else d.AddMember("value", rapidjson::Value(), a);
        if (q.unit.empty()) d.AddMember("unit", rapidjson::Value(), a);
        else d.AddMember("unit", rapidjson::Value(q.unit.c_str(), a), a);
        if (fam.empty()) d.AddMember("family", rapidjson::Value(), a);
        else d.AddMember("family", rapidjson::Value(fam.c_str(), a), a);

        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        d.Accept(w);
        std::printf("%s\n", buf.GetString());
    }
    return 0;
}

int cmd_token(std::int64_t user_id, const mirobody::optional<std::string>& config_path) {
    mirobody::Config cfg;
    try {
        cfg = mirobody::load_config(config_path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "token: failed to load config: %s\n", e.what());
        return 1;
    }
    if (cfg.jwt.key.empty()) {
        std::fprintf(stderr, "token: JWT_KEY is not configured\n");
        return 1;
    }
    mirobody::jwt::JwtHs256::Options o;
    o.key = cfg.jwt.key;
    o.salt = cfg.jwt.salt;
    if (!cfg.jwt.iss.empty())       o.iss       = cfg.jwt.iss;
    if (!cfg.jwt.aud.empty())       o.aud       = cfg.jwt.aud;
    if (!cfg.jwt.client_id.empty()) o.client_id = cfg.jwt.client_id;
    if (!cfg.jwt.scope.empty())     o.scope     = cfg.jwt.scope;
    mirobody::jwt::JwtHs256 j(std::move(o));
    std::printf("%s\n", j.generate(user_id).c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);            // emit UTF-8 to the console
    (void)argc; (void)argv;
    std::vector<std::string> args = utf8_args();
#else
    std::vector<std::string> args(argv, argv + argc);
#endif
    const char* prog = args.empty() ? "fhir" : args[0].c_str();
    if (args.size() < 2) { print_usage(prog); return 2; }
    std::string cmd = args[1];

    if (cmd == "-h" || cmd == "--help") { print_usage(prog); return 0; }

    if (cmd == "normalize" || cmd == "parse") {
        std::vector<std::string> terms(args.begin() + 2, args.end());
        return cmd_normalize(terms);
    }

    if (cmd == "token") {
        std::int64_t user_id = 0;
        mirobody::optional<std::string> config_path;
        bool have_uid = false;
        for (size_t i = 2; i < args.size(); ++i) {
            const std::string& a = args[i];
            if (a == "--config" && i + 1 < args.size()) { config_path = args[++i]; }
            else if (!have_uid) { user_id = std::strtoll(a.c_str(), nullptr, 10); have_uid = true; }
        }
        if (!have_uid) { std::fprintf(stderr, "token: missing <user_id>\n"); return 2; }
        return cmd_token(user_id, config_path);
    }

    std::fprintf(stderr, "unknown subcommand: %s\n", cmd.c_str());
    print_usage(prog);
    return 2;
}
