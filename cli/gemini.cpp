// Standalone debugger for GeminiClient — both AI Studio and Vertex paths.
//
//   # AI Studio (default mode)
//   gemini --model gemini-2.5-flash "summarize this"
//
//   # Vertex AI
//   gemini --mode vertex --gcp-project my-proj --gcp-location us-central1 "..."
//
// Config precedence: --flag > env > YAML (local / remote) > builtin default.
//
// YAML keys honored:
//   GEMINI_MODE                 ("ai-studio" or "vertex")
//   GOOGLE_API_KEY              AI Studio bearer
//   GCP_ACCESS_TOKEN            Vertex bearer (gcloud auth print-access-token)
//   GOOGLE_CLOUD_PROJECT        Vertex
//   GOOGLE_CLOUD_LOCATION       Vertex
//   GEMINI_AI_STUDIO_BASE_URL   override AiStudio host
//   GEMINI_VERTEX_BASE_URL      override Vertex host
//   GEMINI_API_VERSION          AiStudio default v1beta
//   GEMINI_MODEL
//   GEMINI_TEMPERATURE
//   GEMINI_CONNECT_TIMEOUT_MS, GEMINI_REQUEST_TIMEOUT_MS
//   GEMINI_INPUT_PRICE, GEMINI_OUTPUT_PRICE,
//   GEMINI_MIN_CHUNK_SIZE

#include "client/http_client.hpp"
#include "config/config.hpp"
#include "llm/gemini.hpp"
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
        "Standalone Gemini streamGenerateContent debug client.\n"
        "Pick `--mode ai-studio` (default) for AI Studio (API-key auth) or\n"
        "`--mode vertex` for Vertex AI (OAuth-token auth).\n"
        "\n"
        "Options:\n"
        "  --config <path>            Local YAML to load.\n"
        "  --mode <ai-studio|vertex>  Override GEMINI_MODE. Default: ai-studio\n"
        "  --api-key <key>            AI Studio: overrides GOOGLE_API_KEY.\n"
        "  --access-token <tok>       Vertex: overrides GCP_ACCESS_TOKEN.\n"
        "  --gcp-project <id>         Vertex: overrides GOOGLE_CLOUD_PROJECT.\n"
        "  --gcp-location <region>    Vertex: overrides GOOGLE_CLOUD_LOCATION. Default: us-central1\n"
        "  --model <name>             Override GEMINI_MODEL. Default: gemini-2.5-flash\n"
        "  --api-version <ver>        AiStudio: default v1beta. Vertex always uses v1.\n"
        "  --system <text>            System instruction.\n"
        "  --temperature <f>          Default: 0.1\n"
        "  --message <text>           User message; repeatable.\n"
        "  --min-chunk-size <n>       Reply-text coalescing threshold.\n"
        "  --connect-timeout-ms <n>   HTTP connect timeout.\n"
        "  --request-timeout-ms <n>   HTTP request timeout.\n"
        "  --input-price <usd>        $/M input tokens.\n"
        "  --output-price <usd>       $/M output tokens.\n"
        "  --raw                      One TSV line per event.\n"
        "  --no-color                 Disable ANSI escapes.\n"
        "  -h, --help                 Show this help.\n",
        prog);
}

bool parse_mode(const std::string& s, mirobody::llm::GeminiMode& out) {
    if (s == "ai-studio" || s == "aistudio") { out = mirobody::llm::GeminiMode::AiStudio; return true; }
    if (s == "vertex"    || s == "vertexai") { out = mirobody::llm::GeminiMode::Vertex;   return true; }
    return false;
}

//------------------------------------------------------------------------------

struct Args {
    mirobody::llm::GeminiOptions opt;
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
        std::string m = store.get_str("GEMINI_MODE");
        if (!m.empty() && !parse_mode(m, out.opt.mode)) {
            std::fprintf(stderr, "GEMINI_MODE must be ai-studio or vertex, got %s\n", m.c_str());
            return 2;
        }
    }

    // Google's own names where Google has one. Same set the server reads, so one
    // configuration points both at the same surface -- see res/agents/baseline.cpp.
    // (The CLI takes a token directly; the server also resolves one through ADC
    // when none is given.)
    out.opt.api_key            = store.get_str("GOOGLE_API_KEY",            out.opt.api_key);
    out.opt.access_token       = store.get_str("GCP_ACCESS_TOKEN",          out.opt.access_token);
    out.opt.gcp_project        = store.get_str("GOOGLE_CLOUD_PROJECT",      out.opt.gcp_project);
    out.opt.gcp_location       = store.get_str("GOOGLE_CLOUD_LOCATION",     out.opt.gcp_location);
    out.opt.ai_studio_base_url = store.get_str("GEMINI_AI_STUDIO_BASE_URL", out.opt.ai_studio_base_url);
    out.opt.vertex_base_url    = store.get_str("GEMINI_VERTEX_BASE_URL",    out.opt.vertex_base_url);
    out.opt.api_version        = store.get_str("GEMINI_API_VERSION",        out.opt.api_version);
    out.opt.model              = store.get_str("GEMINI_MODEL",              out.opt.model);
    { std::string s = store.get_str("GEMINI_TEMPERATURE"); if (!s.empty()) out.opt.temperature = std::atof(s.c_str()); }
    out.opt.connect_timeout_ms = static_cast<int>(
        store.get_int("GEMINI_CONNECT_TIMEOUT_MS", out.opt.connect_timeout_ms));
    out.opt.request_timeout_ms = static_cast<int>(
        store.get_int("GEMINI_REQUEST_TIMEOUT_MS", out.opt.request_timeout_ms));
    std::int64_t n = store.get_int("GEMINI_MIN_CHUNK_SIZE", 0);
    if (n > 0) {
        out.opt.min_chunk_size = static_cast<size_t>(n);
    }
    { std::string s = store.get_str("GEMINI_INPUT_PRICE"); if (!s.empty()) out.opt.input_price  = std::atof(s.c_str()); }
    { std::string s = store.get_str("GEMINI_OUTPUT_PRICE"); if (!s.empty()) out.opt.output_price = std::atof(s.c_str()); }

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
                std::fprintf(stderr, "--mode must be ai-studio or vertex\n");
                return 2;
            }
        }
        else if (a == "--api-key")               { out.opt.api_key            = need(i, "--api-key"); }
        else if (a == "--access-token")          { out.opt.access_token       = need(i, "--access-token"); }
        else if (a == "--gcp-project")           { out.opt.gcp_project        = need(i, "--gcp-project"); }
        else if (a == "--gcp-location")          { out.opt.gcp_location       = need(i, "--gcp-location"); }
        else if (a == "--model")                 { out.opt.model              = need(i, "--model"); }
        else if (a == "--api-version")           { out.opt.api_version        = need(i, "--api-version"); }
        else if (a == "--system")                { out.system_prompt          = need(i, "--system"); }
        else if (a == "--temperature")           { out.opt.temperature        = std::atof(need(i, "--temperature").c_str()); }
        else if (a == "--message")               { out.messages.push_back(need(i, "--message")); }
        else if (a == "--min-chunk-size")        { out.opt.min_chunk_size     = static_cast<size_t>(std::atoll(need(i, "--min-chunk-size").c_str())); }
        else if (a == "--connect-timeout-ms")    { out.opt.connect_timeout_ms = std::atoi(need(i, "--connect-timeout-ms").c_str()); }
        else if (a == "--request-timeout-ms")    { out.opt.request_timeout_ms = std::atoi(need(i, "--request-timeout-ms").c_str()); }
        else if (a == "--input-price")           { out.opt.input_price        = std::atof(need(i, "--input-price").c_str()); }
        else if (a == "--output-price")          { out.opt.output_price       = std::atof(need(i, "--output-price").c_str()); }
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

constexpr const char* kCli = "gemini";

const char* mode_name(mirobody::llm::GeminiMode m) {
    return m == mirobody::llm::GeminiMode::Vertex ? "vertex" : "ai-studio";
}

void log_effective(const Args& args) {
    using mirobody::platform::log_info;
    log_info("%s: mode=%s model=%s api_version=%s temperature=%.3f",
        kCli, mode_name(args.opt.mode),
        args.opt.model.c_str(),
        args.opt.api_version.c_str(),
        args.opt.temperature);
    if (args.opt.mode == mirobody::llm::GeminiMode::AiStudio) {
        log_info("%s: api_key=%s ai_studio_base_url=%s",
            kCli,
            mirobody::tools::mask_secret(args.opt.api_key).c_str(),
            args.opt.ai_studio_base_url.empty() ? "<default>" : args.opt.ai_studio_base_url.c_str());
    } else {
        log_info("%s: access_token=%s project=%s location=%s vertex_base_url=%s",
            kCli,
            mirobody::tools::mask_secret(args.opt.access_token).c_str(),
            args.opt.gcp_project.empty()  ? "<not set>" : args.opt.gcp_project.c_str(),
            args.opt.gcp_location.empty() ? "<not set>" : args.opt.gcp_location.c_str(),
            args.opt.vertex_base_url.empty() ? "<default>" : args.opt.vertex_base_url.c_str());
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
    mirobody::client::HttpClient::global_init();

    log_effective(args);

    std::vector<mirobody::llm::ChatMessage> chat;
    chat.reserve(args.messages.size());
    for (auto& m : args.messages) chat.push_back({"user", std::move(m)});

    mirobody::llm::GeminiClient client{std::move(args.opt)};
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
