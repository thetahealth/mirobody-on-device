// Standalone CLI for the LLM file-text parsers (src/transcode/parser.*).
//
//   file_parser <in> [--parser gemini|qwen] [--mime <type>] [--config <path>]
//
// Reads <in>, runs the configured parser, and prints the extracted text to
// stdout. Credentials and the backend selection come from config / env, exactly
// as the server resolves them:
//   FILE_PARSER         gemini | qwen   (default gemini; --parser overrides)
//   GOOGLE_API_KEY      Gemini (AI Studio) key
//   DASHSCOPE_API_KEY   Qwen (DashScope) key
//
// The MIME type defaults to a guess from the file extension; override with
// --mime for an extensionless or mislabeled file.
//
// Exit status is 0 on success, 1 on usage / I/O / extraction errors.

#include "client/http_client.hpp"
#include "config/config.hpp"
#include "transcode/parser.hpp"
#include "platform/log.hpp"
#include "event_printer.hpp"   // prepare_windows_console (UTF-8 console output)


#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace {

const char* kUsage =
    "Usage:\n"
    "  file_parser <in> [--parser gemini|qwen] [--model <id>] [--mime <type>]\n"
    "                   [--timeout-ms <n>] [--api-key <key>] [--config <path>]\n"
    "\n"
    "  Reads <in>, extracts its text with the selected parser, prints to stdout.\n"
    "  --parser overrides FILE_PARSER; --model overrides the backend model id\n"
    "  (qwen must be a vision model, e.g. qwen-vl-max / qwen-vl-plus).\n"
    "  Keys come from GOOGLE_API_KEY / DASHSCOPE_API_KEY, or pass --api-key.\n";

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// Lowercased file extension (without the dot), or "" if none.
std::string extension(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    const std::size_t sep = path.find_last_of("/\\");
    if (dot == std::string::npos || (sep != std::string::npos && dot < sep)) return std::string();
    std::string ext = path.substr(dot + 1);
    for (std::size_t i = 0; i < ext.size(); ++i) {
        if (ext[i] >= 'A' && ext[i] <= 'Z') ext[i] = static_cast<char>(ext[i] - 'A' + 'a');
    }
    return ext;
}

// Best-effort MIME from the extension; the parser only needs it to label the
// inline data, so an octet-stream fallback is harmless for most providers.
std::string guess_mime(const std::string& path) {
    const std::string e = extension(path);
    if (e == "pdf")                 return "application/pdf";
    if (e == "png")                 return "image/png";
    if (e == "jpg" || e == "jpeg")  return "image/jpeg";
    if (e == "webp")                return "image/webp";
    if (e == "gif")                 return "image/gif";
    if (e == "txt")                 return "text/plain";
    return "application/octet-stream";
}

// Set an environment variable before load_config(), so the config store (which
// overlays env) picks it up -- used to honor --parser / --api-key overrides.
void set_env(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

}  // namespace

int main(int argc, char** argv) {
    mirobody::tools::prepare_windows_console();   // UTF-8 stdout (extracted text is UTF-8)

    std::string in_path, mime, parser_sel, api_key, model;
    std::optional<std::string> config_path;

    auto need = [&](int& i, const char* what) -> std::string {
        if (i + 1 >= argc) { std::fprintf(stderr, "file_parser: %s needs a value\n", what); std::exit(2); }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if      (a == "-h" || a == "--help") { std::fputs(kUsage, stderr); return 0; }
        else if (a == "--parser")            { parser_sel = need(i, "--parser"); }
        else if (a == "--model")             { model      = need(i, "--model"); }
        else if (a == "--mime")              { mime       = need(i, "--mime"); }
        else if (a == "--timeout-ms")        { set_env("FILE_PARSER_TIMEOUT_MS", need(i, "--timeout-ms")); }
        else if (a == "--api-key")           { api_key    = need(i, "--api-key"); }
        else if (a == "--config")            { config_path = need(i, "--config"); }
        else if (!a.empty() && a[0] == '-')  { std::fprintf(stderr, "file_parser: unknown flag '%s'\n", a.c_str()); return 2; }
        else if (in_path.empty())            { in_path = a; }
        else { std::fprintf(stderr, "file_parser: unexpected argument '%s'\n", a.c_str()); return 2; }
    }

    if (in_path.empty()) {
        std::fputs(kUsage, stderr);
        return 1;
    }

    std::string bytes;
    if (!read_file(in_path, bytes)) {
        std::fprintf(stderr, "file_parser: cannot read '%s'\n", in_path.c_str());
        return 1;
    }
    if (mime.empty()) mime = guess_mime(in_path);
    if (!parser_sel.empty()) set_env("FILE_PARSER", parser_sel);

    // --api-key overrides the credential for the selected backend (defaults to
    // gemini when --parser is omitted). Set before load_config so the store
    // picks it up, same as the env vars it shadows.
    const std::string backend = parser_sel.empty() ? "gemini" : parser_sel;
    if (!api_key.empty()) set_env(backend == "qwen" ? "DASHSCOPE_API_KEY" : "GOOGLE_API_KEY", api_key);
    if (!model.empty())   set_env(backend == "qwen" ? "FILE_PARSER_QWEN_MODEL" : "FILE_PARSER_GEMINI_MODEL", model);

    const mirobody::Config cfg = mirobody::load_config(config_path);
    std::unique_ptr<mirobody::file::Parser> parser = mirobody::file::make_parser(cfg);
    if (!parser) {
        std::fprintf(stderr, "file_parser: extraction is disabled "
                             "(FILE_PARSER is unset to none/off or an unknown value)\n");
        return 1;
    }

    // The credential make_parser resolved for this backend, masked -- so a 401
    // is easy to triage as "no key" vs "wrong key".
    const std::string key = std::string(parser->name()) == "qwen"
        ? (cfg.dashscope.api_key.empty() ? cfg.store.get_str("DASHSCOPE_API_KEY") : cfg.dashscope.api_key)
        : cfg.store.get_str("GOOGLE_API_KEY", cfg.gemini.api_key);
    mirobody::platform::log_info("file_parser: parser=%s key=%s mime=%s bytes=%zu file=%s",
                                 parser->name(), mirobody::tools::mask_secret(key).c_str(),
                                 mime.c_str(), bytes.size(), in_path.c_str());

    mirobody::client::HttpClient::global_init();
    const std::string text = parser->extract_text(bytes, mime, in_path);
    mirobody::client::HttpClient::global_cleanup();

    if (text.empty()) {
        std::fprintf(stderr, "file_parser: no text extracted (missing key, transport "
                             "error, or empty result -- see logs above)\n");
        return 1;
    }

    std::fputs(text.c_str(), stdout);
    if (!text.empty() && text[text.size() - 1] != '\n') std::fputc('\n', stdout);
    return 0;
}
