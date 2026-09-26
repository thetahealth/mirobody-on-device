// Standalone debugger for MiroThinkerClient.
//
// Drives src/llm/mirothinker.* end-to-end without involving the
// embedded HTTP/WS server, so new parameters or wire-format quirks can be
// poked at with a one-line rebuild rather than a server restart.
//
//   mirothinker "summarize this in one sentence"
//   mirothinker --system "be terse" --model miro-thinker-mini "..."
//   echo "..." | mirohinker --raw
//
// Configuration precedence (highest to lowest):
//   1. command-line --flag
//   2. environment variable matching the YAML key (e.g. MIROTHINKER_API_KEY)
//   3. YAML key — local file (--config / MIROBODY_CONFIG / ./config.yml)
//                 or remote (CONFIG_SERVER + CONFIG_TOKEN + ENV)
//   4. compiled-in default
//
// YAML keys honored:
//   MIROTHINKER_API_KEY, MCP_PUBLIC_URL,
//   MIROTHINKER_BASE_URL, MIROTHINKER_MODEL, MIROTHINKER_MCP_NAME,
//   MIROTHINKER_CONNECT_TIMEOUT_MS, MIROTHINKER_REQUEST_TIMEOUT_MS,
//   MIROTHINKER_INPUT_PRICE, MIROTHINKER_OUTPUT_PRICE,
//   MIROTHINKER_MIN_CHUNK_SIZE

#include "client/http_client.hpp"
#include "config/config.hpp"
#include "llm/mirothinker.hpp"
#include "platform/log.hpp"
#include "event_printer.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [options] [\"user message\"]\n"
        "\n"
        "Standalone MiroThinker client for protocol-level debugging.\n"
        "Streams events from api.miromind.ai (or --base-url) to stdout.\n"
        "\n"
        "Options:\n"
        "  --config <path>           Local YAML to load (overrides MIROBODY_CONFIG / ./config.yml).\n"
        "  --api-key <key>           Override MIROTHINKER_API_KEY.\n"
        "  --mcp-url <url>           Override MCP_PUBLIC_URL.\n"
        "  --base-url <url>          Override MIROTHINKER_BASE_URL. Default: https://api.miromind.ai/v1\n"
        "  --model <name>            Override MIROTHINKER_MODEL. Default: miro-thinker\n"
        "  --mcp-name <name>         Override MIROTHINKER_MCP_NAME. Default: theta_health\n"
        "  --system <text>           System prompt prepended to messages.\n"
        "  --message <text>          User message; repeatable.\n"
        "  --min-chunk-size <n>      Reply-text coalescing threshold.\n"
        "  --connect-timeout-ms <n>  HTTP connect timeout.\n"
        "  --request-timeout-ms <n>  HTTP request timeout.\n"
        "  --input-price <usd>       $/M input tokens (for cost stats).\n"
        "  --output-price <usd>      $/M output tokens.\n"
        "  --raw                     One TSV line per event.\n"
        "  --no-color                Disable ANSI escapes in pretty mode.\n"
        "  -h, --help                Show this help.\n"
        "\n"
        "Config loaded from remote (CONFIG_SERVER+CONFIG_TOKEN+ENV) and/or YAML;\n"
        "see the source header for the full list of keys honored.\n",
        prog);
}

//------------------------------------------------------------------------------

struct Args {
    mirobody::llm::MiroThinkerOptions opt;
    std::string system_prompt;
    std::vector<std::string> messages;
    bool raw_mode = false;
    bool color   = true;
};

int parse_args(int argc, char** argv, Args& out) {
    // First pass: find --config (and bail on --help / unknown options).
    std::optional<std::string> config_path;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { print_usage(argv[0]); return 1; }
        else if (a == "--config") {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for --config\n"); return 2; }
            config_path = argv[++i];
        }
    }

    // Load the key-value store. utils::ConfigStore::get_str falls back to env
    // vars before YAML, so the precedence here is env > YAML > builtin.
    auto store = mirobody::load_config_store(config_path);

    out.opt.api_key         = store.get_str("MIROTHINKER_API_KEY",  out.opt.api_key);
    out.opt.mcp_url         = store.get_str("MCP_PUBLIC_URL",       out.opt.mcp_url);
    out.opt.base_url        = store.get_str("MIROTHINKER_BASE_URL", out.opt.base_url);
    out.opt.model           = store.get_str("MIROTHINKER_MODEL",    out.opt.model);
    out.opt.mcp_server_name = store.get_str("MIROTHINKER_MCP_NAME", out.opt.mcp_server_name);

    out.opt.connect_timeout_ms = static_cast<int>(
        store.get_int("MIROTHINKER_CONNECT_TIMEOUT_MS", out.opt.connect_timeout_ms));
    out.opt.request_timeout_ms = static_cast<int>(
        store.get_int("MIROTHINKER_REQUEST_TIMEOUT_MS", out.opt.request_timeout_ms));
    std::int64_t n = store.get_int("MIROTHINKER_MIN_CHUNK_SIZE", 0);
    if (n > 0) {
        out.opt.min_chunk_size = static_cast<size_t>(n);
    }
    { std::string s = store.get_str("MIROTHINKER_INPUT_PRICE"); if (!s.empty()) out.opt.input_price  = std::atof(s.c_str()); }
    { std::string s = store.get_str("MIROTHINKER_OUTPUT_PRICE"); if (!s.empty()) out.opt.output_price = std::atof(s.c_str()); }

    // Second pass: --flag overrides on top of what the store provided.
    std::string positional;
    auto need = [&](int& i, const char* what) -> std::string {
        if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", what); std::exit(2); }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "-h" || a == "--help") { /* handled in first pass */ }
        else if (a == "--config")            { ++i; /* consumed in first pass */ }
        else if (a == "--api-key")           { out.opt.api_key            = need(i, "--api-key"); }
        else if (a == "--mcp-url")           { out.opt.mcp_url            = need(i, "--mcp-url"); }
        else if (a == "--base-url")          { out.opt.base_url           = need(i, "--base-url"); }
        else if (a == "--model")             { out.opt.model              = need(i, "--model"); }
        else if (a == "--mcp-name")          { out.opt.mcp_server_name    = need(i, "--mcp-name"); }
        else if (a == "--system")            { out.system_prompt          = need(i, "--system"); }
        else if (a == "--message")           { out.messages.push_back(need(i, "--message")); }
        else if (a == "--min-chunk-size")    { out.opt.min_chunk_size     = static_cast<size_t>(std::atoll(need(i, "--min-chunk-size").c_str())); }
        else if (a == "--connect-timeout-ms"){ out.opt.connect_timeout_ms = std::atoi(need(i, "--connect-timeout-ms").c_str()); }
        else if (a == "--request-timeout-ms"){ out.opt.request_timeout_ms = std::atoi(need(i, "--request-timeout-ms").c_str()); }
        else if (a == "--input-price")       { out.opt.input_price        = std::atof(need(i, "--input-price").c_str()); }
        else if (a == "--output-price")      { out.opt.output_price       = std::atof(need(i, "--output-price").c_str()); }
        else if (a == "--raw")               { out.raw_mode = true; out.color = false; }
        else if (a == "--no-color")          { out.color = false; }
        else if (!a.empty() && a.front() == '-') {
            std::fprintf(stderr, "unknown option: %.*s\n", static_cast<int>(a.size()), a.data());
            print_usage(argv[0]); return 2;
        }
        else                                 { positional = std::string{a}; }
    }

    if (out.messages.empty()) {
        if (!positional.empty()) {
            out.messages.push_back(std::move(positional));
        } else if (!mirobody::tools::stdin_is_tty()) {
            // Piped input: read until EOF. TTYs would block silently, so we
            // fall through to the usage hint below instead.
            std::string s = mirobody::tools::read_stdin_to_eof();
            if (!s.empty()) out.messages.push_back(std::move(s));
        }
    }

    if (out.messages.empty()) {
        std::fprintf(stderr,
            "no user message - pass one as a positional arg, --message, or pipe via stdin:\n"
            "  %s \"your question here\"\n"
            "  %s --message \"...\"\n"
            "  echo \"...\" | %s\n"
            "See --help for all options.\n",
            argv[0], argv[0], argv[0]);
        return 2;
    }
    return 0;
}

constexpr const char* kCli = "mirothinker";

void log_effective(const Args& args) {
    using mirobody::platform::log_info;
    log_info("%s: model=%s base_url=%s mcp_name=%s",
        kCli,
        args.opt.model.c_str(),
        args.opt.base_url.c_str(),
        args.opt.mcp_server_name.c_str());
    log_info("%s: api_key=%s mcp_url=%s",
        kCli,
        mirobody::tools::mask_secret(args.opt.api_key).c_str(),
        args.opt.mcp_url.empty() ? "<not set>" : args.opt.mcp_url.c_str());
    log_info("%s: timeouts connect=%dms request=%dms; min_chunk=%zu; pricing in=%.6g out=%.6g",
        kCli,
        args.opt.connect_timeout_ms, args.opt.request_timeout_ms,
        args.opt.min_chunk_size,
        args.opt.input_price, args.opt.output_price);
    log_info("%s: sending %zu message(s), system_prompt=%zu bytes",
        kCli, args.messages.size(), args.system_prompt.size());
}

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

    log_effective(args);

    std::vector<mirobody::llm::ChatMessage> chat;
    chat.reserve(args.messages.size());
    for (auto& m : args.messages) {
        chat.push_back({"user", std::move(m)});
    }

    mirobody::llm::MiroThinkerClient client{std::move(args.opt)};
    mirobody::tools::EventPrinter printer{args.raw_mode, args.color};

    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = client.ainvoke(chat, args.system_prompt, std::ref(printer));
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();

    if (!args.raw_mode) std::fputc('\n', stdout);

    mirobody::platform::log_info("%s: complete in %.2fs, ok=%s",
        kCli, elapsed, ok ? "true" : "false");

    mirobody::client::HttpClient::global_cleanup();
    return ok ? 0 : 1;
}
