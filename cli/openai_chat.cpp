// Standalone debugger for OpenAIChatClient.
//
// Works against any OpenAI-compatible /chat/completions endpoint — flip
// --base-url to target OpenRouter, Nebula, DashScope, Ark, vLLM, llama.cpp, etc.
//
//   openai_chat "hello"
//   openai_chat --base-url https://openrouter.ai/api/v1 --model openai/gpt-4o-mini "..."
//
// Config precedence: --flag > env > YAML (local / remote) > builtin default.
//
// YAML keys honored:
//   OPENAI_API_KEY, OPENAI_BASE_URL, OPENAI_CHAT_MODEL,
//   OPENAI_CHAT_CONNECT_TIMEOUT_MS, OPENAI_CHAT_REQUEST_TIMEOUT_MS,
//   OPENAI_CHAT_INPUT_PRICE, OPENAI_CHAT_OUTPUT_PRICE,
//   OPENAI_CHAT_MIN_CHUNK_SIZE
//
// Azure OpenAI is auto-detected: when AZURE_OPENAI_ENDPOINT is set the CLI
// flips to Azure mode and uses AZURE_OPENAI_KEY (or AZURE_OPENAI_API_KEY) for auth.
//
// Azure-mode YAML keys (or matching --flag):
//   AZURE_OPENAI_KEY (alias AZURE_OPENAI_API_KEY), AZURE_OPENAI_ENDPOINT,
//   AZURE_OPENAI_DEPLOYMENT, AZURE_OPENAI_API_VERSION

#include "client/http_client.hpp"
#include "config/config.hpp"
#include "llm/openai_chat.hpp"
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
        "OpenAI-compatible Chat Completions debug client.\n"
        "\n"
        "Options:\n"
        "  --config <path>           Local YAML to load.\n"
        "  --api-key <key>           Override OPENAI_API_KEY (or AZURE_OPENAI_KEY in Azure mode).\n"
        "  --base-url <url>          Override OPENAI_BASE_URL. Default: https://api.openai.com/v1\n"
        "  --model <name>            Override OPENAI_CHAT_MODEL. Default: gpt-4o-mini\n"
        "  --azure-endpoint <url>    Switch to Azure mode; e.g. https://my.openai.azure.com\n"
        "  --azure-deployment <name> Deployment name for Azure mode.\n"
        "  --azure-api-version <ver> Azure API version. Default: 2024-10-21\n"
        "  --system <text>           System prompt prepended to messages.\n"
        "  --message <text>          User message; repeatable.\n"
        "  --min-chunk-size <n>      Reply-text coalescing threshold.\n"
        "  --connect-timeout-ms <n>  HTTP connect timeout.\n"
        "  --request-timeout-ms <n>  HTTP request timeout.\n"
        "  --input-price <usd>       $/M input tokens.\n"
        "  --output-price <usd>      $/M output tokens.\n"
        "  --raw                     One TSV line per event.\n"
        "  --no-color                Disable ANSI escapes in pretty mode.\n"
        "  -h, --help                Show this help.\n",
        prog);
}

//------------------------------------------------------------------------------

struct Args {
    mirobody::llm::OpenAIChatOptions opt;
    std::string system_prompt;
    std::vector<std::string> messages;
    bool raw_mode = false;
    bool color   = true;
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
    out.opt.api_key  = store.get_str("OPENAI_API_KEY",     out.opt.api_key);
    out.opt.base_url = store.get_str("OPENAI_BASE_URL",    out.opt.base_url);
    out.opt.model    = store.get_str("OPENAI_CHAT_MODEL",  out.opt.model);
    // Azure-mode fields, sourced from the shared AzureConfig loader so the
    // AZURE_OPENAI_* key names and key-alias precedence match the server and the
    // realtime CLI. The mode itself is auto-flipped further down once everything
    // (including --flag overrides) has been collected.
    {
        mirobody::AzureConfig az = mirobody::load_azure_config(store);
        out.opt.azure_endpoint    = az.endpoint;
        out.opt.azure_deployment  = az.deployment;
        out.opt.azure_api_version = az.api_version;
        if (!az.key.empty()) out.opt.api_key = std::move(az.key);
    }
    out.opt.connect_timeout_ms = static_cast<int>(
        store.get_int("OPENAI_CHAT_CONNECT_TIMEOUT_MS", out.opt.connect_timeout_ms));
    out.opt.request_timeout_ms = static_cast<int>(
        store.get_int("OPENAI_CHAT_REQUEST_TIMEOUT_MS", out.opt.request_timeout_ms));
    std::int64_t n = store.get_int("OPENAI_CHAT_MIN_CHUNK_SIZE", 0);
    if (n > 0) {
        out.opt.min_chunk_size = static_cast<size_t>(n);
    }
    { std::string s = store.get_str("OPENAI_CHAT_INPUT_PRICE"); if (!s.empty()) out.opt.input_price  = std::atof(s.c_str()); }
    { std::string s = store.get_str("OPENAI_CHAT_OUTPUT_PRICE"); if (!s.empty()) out.opt.output_price = std::atof(s.c_str()); }

    std::string positional;
    auto need = [&](int& i, const char* what) -> std::string {
        if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", what); std::exit(2); }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "-h" || a == "--help") {}
        else if (a == "--config")             { ++i; }
        else if (a == "--api-key")            { out.opt.api_key            = need(i, "--api-key"); }
        else if (a == "--base-url")           { out.opt.base_url           = need(i, "--base-url"); }
        else if (a == "--model")              { out.opt.model              = need(i, "--model"); }
        else if (a == "--azure-endpoint")     { out.opt.azure_endpoint     = need(i, "--azure-endpoint"); }
        else if (a == "--azure-deployment")   { out.opt.azure_deployment   = need(i, "--azure-deployment"); }
        else if (a == "--azure-api-version")  { out.opt.azure_api_version  = need(i, "--azure-api-version"); }
        else if (a == "--system")             { out.system_prompt          = need(i, "--system"); }
        else if (a == "--message")            { out.messages.push_back(need(i, "--message")); }
        else if (a == "--min-chunk-size")     { out.opt.min_chunk_size     = static_cast<size_t>(std::atoll(need(i, "--min-chunk-size").c_str())); }
        else if (a == "--connect-timeout-ms") { out.opt.connect_timeout_ms = std::atoi(need(i, "--connect-timeout-ms").c_str()); }
        else if (a == "--request-timeout-ms") { out.opt.request_timeout_ms = std::atoi(need(i, "--request-timeout-ms").c_str()); }
        else if (a == "--input-price")        { out.opt.input_price        = std::atof(need(i, "--input-price").c_str()); }
        else if (a == "--output-price")       { out.opt.output_price       = std::atof(need(i, "--output-price").c_str()); }
        else if (a == "--raw")                { out.raw_mode = true; out.color = false; }
        else if (a == "--no-color")           { out.color = false; }
        else if (!a.empty() && a.front() == '-') {
            std::fprintf(stderr, "unknown option: %.*s\n", static_cast<int>(a.size()), a.data());
            print_usage(argv[0]); return 2;
        }
        else                                  { positional = std::string{a}; }
    }

    // Auto-detect Azure mode: if an Azure endpoint is set anywhere
    // (YAML, env via the store, or --azure-endpoint), prefer it.
    if (!out.opt.azure_endpoint.empty()) {
        out.opt.mode = mirobody::llm::OpenAIMode::Azure;
    }

    if (out.messages.empty()) {
        if (!positional.empty()) {
            out.messages.push_back(std::move(positional));
        } else if (!mirobody::tools::stdin_is_tty()) {
            std::string s = mirobody::tools::read_stdin_to_eof();
            if (!s.empty()) out.messages.push_back(std::move(s));
        }
    }
    if (out.messages.empty()) {
        std::fprintf(stderr,
            "no user message - pass one as a positional arg, --message, or pipe via stdin:\n"
            "  %s \"your question here\"\n"
            "  echo \"...\" | %s\n"
            "See --help for all options.\n",
            argv[0], argv[0]);
        return 2;
    }
    return 0;
}

constexpr const char* kCli = "openai_chat";

void log_effective(const Args& args) {
    using mirobody::platform::log_info;
    if (args.opt.mode == mirobody::llm::OpenAIMode::Azure) {
        log_info("%s: mode=azure endpoint=%s deployment=%s api_version=%s",
            kCli,
            args.opt.azure_endpoint.c_str(),
            args.opt.azure_deployment.c_str(),
            args.opt.azure_api_version.c_str());
    } else {
        log_info("%s: model=%s base_url=%s",
            kCli, args.opt.model.c_str(), args.opt.base_url.c_str());
    }
    log_info("%s: api_key=%s",
        kCli, mirobody::tools::mask_secret(args.opt.api_key).c_str());
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
    { int rc = parse_args(argc, argv, args); if (rc != 0) return rc == 1 ? 0 : rc; }

    mirobody::tools::prepare_windows_console();
    mirobody::client::HttpClient::global_init();

    log_effective(args);

    std::vector<mirobody::llm::ChatMessage> chat;
    chat.reserve(args.messages.size());
    for (auto& m : args.messages) chat.push_back({"user", std::move(m)});

    mirobody::llm::OpenAIChatClient client{std::move(args.opt)};
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
