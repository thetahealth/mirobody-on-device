// Standalone debugger for OpenAIRealtimeClient -- the OpenAI Realtime API
// ("GPT realtime") over WebSocket, text-in / text-out. Both OpenAI and Azure
// paths.
//
//   # OpenAI (default mode)
//   openai_realtime --model gpt-realtime "summarize this"
//
//   # Azure OpenAI
//   openai_realtime --mode azure --azure-endpoint wss://my.openai.azure.com \
//                   --azure-deployment my-realtime "..."
//
// Config precedence: --flag > env > YAML (local / remote) > builtin default.
//
// YAML keys honored:
//   OPENAI_REALTIME_MODE              ("openai" or "azure")
//   OPENAI_API_KEY                    OpenAI bearer
//   AZURE_OPENAI_KEY                  Azure api-key (alias AZURE_OPENAI_API_KEY;
//                                     falls back to OPENAI_API_KEY)
//   AZURE_OPENAI_ENDPOINT             Azure host (wss://{resource}.openai.azure.com)
//   AZURE_OPENAI_DEPLOYMENT           Azure deployment name
//   OPENAI_REALTIME_API_VERSION       Azure api-version (default 2024-10-01-preview)
//   OPENAI_REALTIME_OPENAI_BASE_URL   override OpenAI host (wss://...)
//   OPENAI_REALTIME_MODEL
//   OPENAI_REALTIME_TEMPERATURE
//   OPENAI_REALTIME_CONNECT_TIMEOUT_MS, OPENAI_REALTIME_REQUEST_TIMEOUT_MS
//   OPENAI_REALTIME_INPUT_PRICE, OPENAI_REALTIME_OUTPUT_PRICE,
//   OPENAI_REALTIME_MIN_CHUNK_SIZE

#include "config/config.hpp"
#include "llm/openai_realtime.hpp"
#include "platform/log.hpp"
#include "event_printer.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include "compat/cxx11.hpp"
#include <string>
#include <utility>
#include <vector>

namespace {

void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [options] [\"user message\"]\n"
        "\n"
        "Standalone OpenAI Realtime API (\"GPT realtime\") debug client, text mode.\n"
        "Pick `--mode openai` (default) for OpenAI (bearer auth) or\n"
        "`--mode azure` for Azure OpenAI (api-key auth).\n"
        "\n"
        "Options:\n"
        "  --config <path>            Local YAML to load.\n"
        "  --mode <openai|azure>      Override OPENAI_REALTIME_MODE. Default: openai\n"
        "  --api-key <key>            Overrides OPENAI_API_KEY / AZURE_OPENAI_API_KEY.\n"
        "  --azure-endpoint <url>     Azure: wss://{resource}.openai.azure.com\n"
        "  --azure-deployment <name>  Azure: deployment name.\n"
        "  --api-version <ver>        Azure api-version. Default 2024-10-01-preview.\n"
        "  --base-url <url>           OpenAI: override host (wss://...).\n"
        "  --model <name>             Override OPENAI_REALTIME_MODEL. Default: gpt-realtime\n"
        "  --no-beta-header           Don't send the OpenAI-Beta: realtime=v1 header.\n"
        "  --system <text>            System instruction.\n"
        "  --temperature <f>          Sent only when > 0. Default: 0 (unset)\n"
        "  --message <text>           User message; repeatable.\n"
        "  --min-chunk-size <n>       Reply-text coalescing threshold.\n"
        "  --connect-timeout-ms <n>   WebSocket connect timeout.\n"
        "  --request-timeout-ms <n>   Whole-exchange timeout.\n"
        "  --input-price <usd>        $/M input tokens.\n"
        "  --output-price <usd>       $/M output tokens.\n"
        "  --insecure                 wss://: skip cert/hostname verification (debug only).\n"
        "  --raw                      One TSV line per event.\n"
        "  --no-color                 Disable ANSI escapes.\n"
        "  -h, --help                 Show this help.\n",
        prog);
}

bool parse_mode(const std::string& s, mirobody::llm::OpenAIRealtimeMode& out) {
    if (s == "openai")             { out = mirobody::llm::OpenAIRealtimeMode::OpenAI; return true; }
    if (s == "azure" || s == "azureopenai") { out = mirobody::llm::OpenAIRealtimeMode::Azure; return true; }
    return false;
}

//------------------------------------------------------------------------------

struct Args {
    mirobody::llm::OpenAIRealtimeOptions opt;
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

    {
        std::string m = store.get_str("OPENAI_REALTIME_MODE");
        if (!m.empty() && !parse_mode(m, out.opt.mode)) {
            std::fprintf(stderr, "OPENAI_REALTIME_MODE must be openai or azure, got %s\n", m.c_str());
            return 2;
        }
    }

    // Azure resource (key/endpoint/deployment) from the shared AzureConfig loader
    // so the key name matches the server and the chat CLI; the Azure key takes
    // precedence in azure mode, else fall back to OPENAI_API_KEY. api_version is
    // realtime-specific (OPENAI_REALTIME_API_VERSION, preview default), so it is
    // read separately rather than from AzureConfig.
    out.opt.api_key          = store.get_str("OPENAI_API_KEY", out.opt.api_key);
    {
        mirobody::AzureConfig az = mirobody::load_azure_config(store);
        if (!az.key.empty()) out.opt.api_key = std::move(az.key);
        out.opt.azure_endpoint   = az.endpoint;
        out.opt.azure_deployment = az.deployment;
    }
    out.opt.openai_base_url  = store.get_str("OPENAI_REALTIME_OPENAI_BASE_URL", out.opt.openai_base_url);
    out.opt.api_version      = store.get_str("OPENAI_REALTIME_API_VERSION",    out.opt.api_version);
    out.opt.model            = store.get_str("OPENAI_REALTIME_MODEL",          out.opt.model);
    { std::string s = store.get_str("OPENAI_REALTIME_TEMPERATURE"); if (!s.empty()) out.opt.temperature = std::atof(s.c_str()); }
    out.opt.connect_timeout_ms = static_cast<int>(
        store.get_int("OPENAI_REALTIME_CONNECT_TIMEOUT_MS", out.opt.connect_timeout_ms));
    out.opt.request_timeout_ms = static_cast<int>(
        store.get_int("OPENAI_REALTIME_REQUEST_TIMEOUT_MS", out.opt.request_timeout_ms));
    std::int64_t n = store.get_int("OPENAI_REALTIME_MIN_CHUNK_SIZE", 0);
    if (n > 0) {
        out.opt.min_chunk_size = static_cast<size_t>(n);
    }
    { std::string s = store.get_str("OPENAI_REALTIME_INPUT_PRICE"); if (!s.empty()) out.opt.input_price  = std::atof(s.c_str()); }
    { std::string s = store.get_str("OPENAI_REALTIME_OUTPUT_PRICE"); if (!s.empty()) out.opt.output_price = std::atof(s.c_str()); }

    std::string positional;
    auto need = [&](int& i, const char* what) -> std::string {
        if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", what); std::exit(2); }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "-h" || a == "--help") {}
        else if (a == "--config")                { ++i; }
        else if (a == "--mode") {
            std::string v = need(i, "--mode");
            if (!parse_mode(v, out.opt.mode)) {
                std::fprintf(stderr, "--mode must be openai or azure\n");
                return 2;
            }
        }
        else if (a == "--api-key")               { out.opt.api_key            = need(i, "--api-key"); }
        else if (a == "--azure-endpoint")        { out.opt.azure_endpoint     = need(i, "--azure-endpoint"); }
        else if (a == "--azure-deployment")      { out.opt.azure_deployment   = need(i, "--azure-deployment"); }
        else if (a == "--api-version")           { out.opt.api_version        = need(i, "--api-version"); }
        else if (a == "--base-url")              { out.opt.openai_base_url    = need(i, "--base-url"); }
        else if (a == "--model")                 { out.opt.model              = need(i, "--model"); }
        else if (a == "--no-beta-header")        { out.opt.beta_header        = false; }
        else if (a == "--system")                { out.system_prompt          = need(i, "--system"); }
        else if (a == "--temperature")           { out.opt.temperature        = std::atof(need(i, "--temperature").c_str()); }
        else if (a == "--message")               { out.messages.push_back(need(i, "--message")); }
        else if (a == "--min-chunk-size")        { out.opt.min_chunk_size     = static_cast<size_t>(std::atoll(need(i, "--min-chunk-size").c_str())); }
        else if (a == "--connect-timeout-ms")    { out.opt.connect_timeout_ms = std::atoi(need(i, "--connect-timeout-ms").c_str()); }
        else if (a == "--request-timeout-ms")    { out.opt.request_timeout_ms = std::atoi(need(i, "--request-timeout-ms").c_str()); }
        else if (a == "--input-price")           { out.opt.input_price        = std::atof(need(i, "--input-price").c_str()); }
        else if (a == "--output-price")          { out.opt.output_price       = std::atof(need(i, "--output-price").c_str()); }
        else if (a == "--insecure")              { out.opt.insecure_skip_verify = true; }
        else if (a == "--raw")                   { out.raw_mode = true; out.color = false; }
        else if (a == "--no-color")              { out.color = false; }
        else if (!a.empty() && a.front() == '-') {
            std::fprintf(stderr, "unknown option: %.*s\n", static_cast<int>(a.size()), a.data());
            print_usage(argv[0]); return 2;
        }
        else                                     { positional = std::string{a}; }
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

constexpr const char* kCli = "openai_realtime";

const char* mode_name(mirobody::llm::OpenAIRealtimeMode m) {
    return m == mirobody::llm::OpenAIRealtimeMode::Azure ? "azure" : "openai";
}

void log_effective(const Args& args) {
    using mirobody::platform::log_info;
    log_info("%s: mode=%s model=%s temperature=%.3f beta_header=%s",
        kCli, mode_name(args.opt.mode),
        args.opt.model.c_str(),
        args.opt.temperature,
        args.opt.beta_header ? "true" : "false");
    if (args.opt.mode == mirobody::llm::OpenAIRealtimeMode::OpenAI) {
        log_info("%s: api_key=%s base_url=%s",
            kCli,
            mirobody::tools::mask_secret(args.opt.api_key).c_str(),
            args.opt.openai_base_url.empty() ? "<default>" : args.opt.openai_base_url.c_str());
    } else {
        log_info("%s: api_key=%s endpoint=%s deployment=%s api_version=%s",
            kCli,
            mirobody::tools::mask_secret(args.opt.api_key).c_str(),
            args.opt.azure_endpoint.empty()   ? "<not set>" : args.opt.azure_endpoint.c_str(),
            args.opt.azure_deployment.empty() ? "<not set>" : args.opt.azure_deployment.c_str(),
            args.opt.api_version.c_str());
    }
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

    log_effective(args);

    std::vector<mirobody::llm::ChatMessage> chat;
    chat.reserve(args.messages.size());
    for (auto& m : args.messages) chat.push_back({"user", std::move(m)});

    mirobody::llm::OpenAIRealtimeClient client{std::move(args.opt)};
    mirobody::tools::EventPrinter printer{args.raw_mode, args.color};

    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = client.ainvoke(chat, args.system_prompt, std::ref(printer));
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();

    if (!args.raw_mode) std::fputc('\n', stdout);

    mirobody::platform::log_info("%s: complete in %.2fs, ok=%s",
        kCli, elapsed, ok ? "true" : "false");

    return ok ? 0 : 1;
}
