// Standalone debugger for OpenAIResponsesClient — the modern OpenAI
// `/responses` SSE protocol with named events.
//
//   openai_responses --model gpt-5-nano "summarize this in one sentence"
//   openai_responses --previous-response-id resp_abc "follow-up question"
//
// Config precedence: --flag > env > YAML (local / remote) > builtin default.
//
// YAML keys honored:
//   OPENAI_API_KEY, OPENAI_BASE_URL, OPENAI_RESPONSES_MODEL,
//   OPENAI_RESPONSES_CONNECT_TIMEOUT_MS, OPENAI_RESPONSES_REQUEST_TIMEOUT_MS,
//   OPENAI_RESPONSES_INPUT_PRICE, OPENAI_RESPONSES_OUTPUT_PRICE,
//   OPENAI_RESPONSES_MIN_CHUNK_SIZE, OPENAI_RESPONSES_STORE

#include "client/http_client.hpp"
#include "config/config.hpp"
#include "llm/openai_responses.hpp"
#include "platform/log.hpp"
#include "event_printer.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include "compat/cxx11.hpp"
#include <string>
#include "compat/cxx11.hpp"
#include <utility>
#include <vector>

namespace {

void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [options] [\"user message\"]\n"
        "\n"
        "OpenAI Responses API debug client (POST /responses, stream=true).\n"
        "\n"
        "Options:\n"
        "  --config <path>             Local YAML to load.\n"
        "  --api-key <key>             Override OPENAI_API_KEY.\n"
        "  --base-url <url>            Override OPENAI_BASE_URL.\n"
        "  --model <name>              Override OPENAI_RESPONSES_MODEL. Default: gpt-5-nano\n"
        "  --system <text>             Instructions field (system prompt).\n"
        "  --message <text>            User message; repeatable.\n"
        "  --previous-response-id <id> Chain onto a prior stored response.\n"
        "  --no-store                  Don't ask the server to retain the response.\n"
        "  --min-chunk-size <n>        Reply-text coalescing threshold.\n"
        "  --connect-timeout-ms <n>    HTTP connect timeout.\n"
        "  --request-timeout-ms <n>    HTTP request timeout.\n"
        "  --input-price <usd>         $/M input tokens.\n"
        "  --output-price <usd>        $/M output tokens.\n"
        "  --raw                       One TSV line per event.\n"
        "  --no-color                  Disable ANSI escapes in pretty mode.\n"
        "  -h, --help                  Show this help.\n",
        prog);
}

//------------------------------------------------------------------------------

struct Args {
    mirobody::llm::OpenAIResponsesOptions opt;
    std::string system_prompt;
    std::vector<std::string> messages;
    bool raw_mode = false;
    bool color   = true;
};

int parse_args(int argc, char** argv, Args& out) {
    mirobody::optional<std::string> config_path;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { print_usage(argv[0]); return 1; }
        else if (a == "--config") {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for --config\n"); return 2; }
            config_path = argv[++i];
        }
    }

    auto store = mirobody::load_config_store(config_path);
    out.opt.api_key  = store.get_str("OPENAI_API_KEY",         out.opt.api_key);
    out.opt.base_url = store.get_str("OPENAI_BASE_URL",        out.opt.base_url);
    out.opt.model    = store.get_str("OPENAI_RESPONSES_MODEL", out.opt.model);
    out.opt.store    = store.get_bool("OPENAI_RESPONSES_STORE", out.opt.store);
    out.opt.connect_timeout_ms = static_cast<int>(
        store.get_int("OPENAI_RESPONSES_CONNECT_TIMEOUT_MS", out.opt.connect_timeout_ms));
    out.opt.request_timeout_ms = static_cast<int>(
        store.get_int("OPENAI_RESPONSES_REQUEST_TIMEOUT_MS", out.opt.request_timeout_ms));
    std::int64_t n = store.get_int("OPENAI_RESPONSES_MIN_CHUNK_SIZE", 0);
    if (n > 0) {
        out.opt.min_chunk_size = static_cast<size_t>(n);
    }
    { std::string s = store.get_str("OPENAI_RESPONSES_INPUT_PRICE"); if (!s.empty()) out.opt.input_price  = std::atof(s.c_str()); }
    { std::string s = store.get_str("OPENAI_RESPONSES_OUTPUT_PRICE"); if (!s.empty()) out.opt.output_price = std::atof(s.c_str()); }

    std::string positional;
    auto need = [&](int& i, const char* what) -> std::string {
        if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", what); std::exit(2); }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "-h" || a == "--help") {}
        else if (a == "--config")                  { ++i; }
        else if (a == "--api-key")                 { out.opt.api_key             = need(i, "--api-key"); }
        else if (a == "--base-url")                { out.opt.base_url            = need(i, "--base-url"); }
        else if (a == "--model")                   { out.opt.model               = need(i, "--model"); }
        else if (a == "--system")                  { out.system_prompt           = need(i, "--system"); }
        else if (a == "--message")                 { out.messages.push_back(need(i, "--message")); }
        else if (a == "--previous-response-id")    { out.opt.previous_response_id= need(i, "--previous-response-id"); }
        else if (a == "--no-store")                { out.opt.store = false; }
        else if (a == "--min-chunk-size")          { out.opt.min_chunk_size      = static_cast<size_t>(std::atoll(need(i, "--min-chunk-size").c_str())); }
        else if (a == "--connect-timeout-ms")      { out.opt.connect_timeout_ms  = std::atoi(need(i, "--connect-timeout-ms").c_str()); }
        else if (a == "--request-timeout-ms")      { out.opt.request_timeout_ms  = std::atoi(need(i, "--request-timeout-ms").c_str()); }
        else if (a == "--input-price")             { out.opt.input_price         = std::atof(need(i, "--input-price").c_str()); }
        else if (a == "--output-price")            { out.opt.output_price        = std::atof(need(i, "--output-price").c_str()); }
        else if (a == "--raw")                     { out.raw_mode = true; out.color = false; }
        else if (a == "--no-color")                { out.color = false; }
        else if (!a.empty() && a.front() == '-') {
            std::fprintf(stderr, "unknown option: %.*s\n", static_cast<int>(a.size()), a.data());
            print_usage(argv[0]); return 2;
        }
        else                                       { positional = std::string{a}; }
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

constexpr const char* kCli = "openai_responses";

void log_effective(const Args& args) {
    using mirobody::platform::log_info;
    log_info("%s: model=%s base_url=%s store=%s",
        kCli, args.opt.model.c_str(), args.opt.base_url.c_str(),
        args.opt.store ? "true" : "false");
    log_info("%s: api_key=%s previous_response_id=%s",
        kCli,
        mirobody::tools::mask_secret(args.opt.api_key).c_str(),
        args.opt.previous_response_id.empty() ? "<none>" : args.opt.previous_response_id.c_str());
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

    mirobody::llm::OpenAIResponsesClient client{std::move(args.opt)};
    mirobody::tools::EventPrinter printer{args.raw_mode, args.color};

    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = client.ainvoke(chat, args.system_prompt, std::ref(printer));
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();

    if (!args.raw_mode) std::fputc('\n', stdout);

    if (ok && !client.last_response_id().empty()) {
        mirobody::platform::log_info("%s: response_id=%s",
            kCli, client.last_response_id().c_str());
    }
    mirobody::platform::log_info("%s: complete in %.2fs, ok=%s",
        kCli, elapsed, ok ? "true" : "false");

    mirobody::client::HttpClient::global_cleanup();
    return ok ? 0 : 1;
}
