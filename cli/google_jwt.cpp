// Standalone verifier for Google ID tokens.
//
// Hands a JWT to jwt::GoogleTokenValidator end-to-end (JWKS fetch + RS256
// verify + iss/aud/exp/email checks) so the validator can be exercised
// without spinning up the server.
//
//   google_jwt "eyJhbGciOi..."
//   google_jwt --client-id 1234.apps.googleusercontent.com "eyJ..."
//   pbpaste | google_jwt
//
// Configuration precedence (highest to lowest):
//   1. command-line --flag
//   2. environment variable matching the YAML key (GOOGLE_CLIENT_ID)
//   3. YAML key - local file (--config / MIROBODY_CONFIG / ./config.yml)
//                 or remote (CONFIG_SERVER + CONFIG_TOKEN + ENV)
//
// YAML keys honored:
//   GOOGLE_CLIENT_ID

#include "client/http_client.hpp"
#include "config/config.hpp"
#include "jwt/google.hpp"
#include "platform/log.hpp"
#include "event_printer.hpp"


#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>

namespace {

void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [options] [\"<id-token>\"]\n"
        "\n"
        "Verify a Google ID token (RS256 JWT) against Google's JWKS.\n"
        "\n"
        "Options:\n"
        "  --config <path>      Local YAML to load (overrides MIROBODY_CONFIG / ./config.yml).\n"
        "  --client-id <id>     Override GOOGLE_CLIENT_ID; required for the `aud` check.\n"
        "  -h, --help           Show this help.\n"
        "\n"
        "The token may also be piped on stdin.\n",
        prog);
}

//------------------------------------------------------------------------------

struct Args {
    std::string client_id;
    std::string id_token;
};

int parse_args(int argc, char** argv, Args& out) {
    std::optional<std::string> config_path;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { print_usage(argv[0]); return 1; }
        else if (a == "--config") {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for --config\n"); return 2; }
            config_path = argv[++i];
        }
    }

    auto store = mirobody::load_config_store(config_path);
    out.client_id = store.get_str("GOOGLE_CLIENT_ID", out.client_id);

    std::string positional;
    auto need = [&](int& i, const char* what) -> std::string {
        if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", what); std::exit(2); }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "-h" || a == "--help") { /* handled above */ }
        else if (a == "--config")            { ++i; /* consumed above */ }
        else if (a == "--client-id")         { out.client_id = need(i, "--client-id"); }
        else if (!a.empty() && a.front() == '-') {
            std::fprintf(stderr, "unknown option: %.*s\n", static_cast<int>(a.size()), a.data());
            print_usage(argv[0]); return 2;
        }
        else                                 { positional = std::string{a}; }
    }

    if (!positional.empty()) {
        out.id_token = std::move(positional);
    } else if (!mirobody::tools::stdin_is_tty()) {
        out.id_token = mirobody::tools::read_stdin_to_eof();
    }

    if (out.client_id.empty()) {
        std::fprintf(stderr,
            "GOOGLE_CLIENT_ID not set - pass --client-id, export GOOGLE_CLIENT_ID, "
            "or add it to config.yml.\n");
        return 2;
    }
    if (out.id_token.empty()) {
        std::fprintf(stderr,
            "no id token - pass one as a positional arg or pipe via stdin:\n"
            "  %s \"eyJhbGciOi...\"\n"
            "  echo \"eyJ...\" | %s\n",
            argv[0], argv[0]);
        return 2;
    }
    return 0;
}

constexpr const char* kCli = "google_jwt";

}

//------------------------------------------------------------------------------

int main(int argc, char** argv) {
    Args args;
    int rc = parse_args(argc, argv, args);
    if (rc != 0) {
        return rc == 1 ? 0 : rc;
    }

    mirobody::tools::prepare_windows_console();
    mirobody::client::HttpClient::global_init();

    mirobody::platform::log_info("%s: client_id=%s, token=%zu bytes",
        kCli, args.client_id.c_str(), args.id_token.size());

    mirobody::jwt::GoogleTokenValidator validator{args.client_id};
    auto result = validator.verify_token(args.id_token);

    mirobody::client::HttpClient::global_cleanup();

    if (!result.ok()) {
        std::fprintf(stderr, "verification failed: %s\n", result.error.c_str());
        return 1;
    }

    std::fwrite(result.claims_json.data(), 1, result.claims_json.size(), stdout);
    std::fputc('\n', stdout);
    return 0;
}
