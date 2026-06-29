// Shared streaming-event pretty-printer for the debug CLIs under cli/.
//
// Header-only so each tool drops it in with a single include; the formatter is
// stateful (groups consecutive Reply / Thinking deltas under a single label),
// so each CLI owns its own Printer instance for the duration of a stream.

#pragma once

#include "llm/event.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace mirobody { namespace tools {

namespace detail {

const char* const kCyan   = "\x1b[36m";
const char* const kYellow = "\x1b[33m";
const char* const kGreen  = "\x1b[32m";
const char* const kRed    = "\x1b[31m";
const char* const kDim    = "\x1b[2m";
const char* const kReset  = "\x1b[0m";

}

// Prepare the Windows console for our output:
//
//   1. Switch its codepage to UTF-8. The CLIs print UTF-8 bytes (source files
//      are saved UTF-8 and MSVC builds with `/utf-8`), but a fresh cmd.exe on
//      a Chinese-locale install defaults to CP936/GBK and renders `—` as
//      `鈥?`. SetConsoleOutputCP/CP(CP_UTF8) is the in-process equivalent of
//      `chcp 65001` — no user setup required.
//
//   2. Enable ENABLE_VIRTUAL_TERMINAL_PROCESSING so our ANSI escapes for
//      colored event labels actually render.
//
// No-op on non-Windows platforms.
inline void prepare_windows_console() {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (h && GetConsoleMode(h, &mode)) {
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    h = GetStdHandle(STD_ERROR_HANDLE);
    if (h && GetConsoleMode(h, &mode)) {
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#endif
}

class EventPrinter {
public:
    EventPrinter(bool raw, bool color) : raw_(raw), color_(color) {}

    bool operator()(const llm::Event& e) {
        using llm::EventType;
        if (raw_) { print_raw(e); return true; }

        switch (e.type) {
            case EventType::Reply:
                if (!reply_started_) {
                    paint(stdout, detail::kGreen, "\n[reply] ");
                    reply_started_ = true;
                }
                std::fputs(e.content.c_str(), stdout);
                std::fflush(stdout);
                break;

            case EventType::Thinking:
                if (!thinking_started_) {
                    paint(stdout, detail::kDim, "\n[thinking] ");
                    thinking_started_ = true;
                }
                std::fputs(e.content.c_str(), stdout);
                std::fflush(stdout);
                break;

            case EventType::QueryTitle:
                reset_inline_groups();
                paint(stdout, detail::kCyan,
                    ("\n[tool:" + e.tool_id + "] " + e.content + "\n").c_str());
                break;

            case EventType::QueryArguments:
                paint(stdout, detail::kDim, ("[args] "   + e.content + "\n").c_str());
                break;

            case EventType::QueryDetail:
                paint(stdout, detail::kDim, ("[result] " + e.content + "\n").c_str());
                break;

            case EventType::CostStatistics:
                reset_inline_groups();
                std::fputc('\n', stdout);
                paint(stdout, detail::kYellow, "[cost] ");
                std::printf("model=%s input=%lld output=%lld thought=%lld total=%lld cost=$%.6f\n",
                    e.cost.model.c_str(),
                    static_cast<long long>(e.cost.input_tokens),
                    static_cast<long long>(e.cost.output_tokens),
                    static_cast<long long>(e.cost.thought_tokens),
                    static_cast<long long>(e.cost.total_tokens),
                    e.cost.total_cost);
                break;

            case EventType::Error:
                reset_inline_groups();
                paint(stderr, detail::kRed, ("\n[error] " + e.content + "\n").c_str());
                break;
        }
        return true;
    }

private:
    void print_raw(const llm::Event& e) {
        std::printf("%s\t%s\t", llm::to_string(e.type), e.tool_id.c_str());
        for (char ch : e.content) {
            switch (ch) {
                case '\n': std::fputs("\\n", stdout);  break;
                case '\r': std::fputs("\\r", stdout);  break;
                case '\t': std::fputs("\\t", stdout);  break;
                case '\\': std::fputs("\\\\", stdout); break;
                default:   std::fputc(ch, stdout);     break;
            }
        }
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }

    void paint(std::FILE* f, const char* color, const char* text) {
        if (color_) std::fputs(color, f);
        std::fputs(text, f);
        if (color_) std::fputs(detail::kReset, f);
        std::fflush(f);
    }

    void reset_inline_groups() {
        reply_started_    = false;
        thinking_started_ = false;
    }

    bool raw_;
    bool color_;
    bool reply_started_    = false;
    bool thinking_started_ = false;
};

// Pulls env var or returns an empty string; helper shared by every CLI.
inline std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string{v} : std::string{};
}

// True when stdin is attached to an interactive terminal (no piped input).
// Lets the CLIs skip a blocking read on stdin when invoked with no message,
// turning a silent hang into an actionable usage hint.
inline bool stdin_is_tty() {
#if defined(_WIN32)
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

// Match the masking style used by Config::print() in src/config/config.cpp:
// first 3 + last 3 chars retained, middle replaced with `******`. Anything
// shorter than 6 chars becomes a fixed `************` so we never leak a
// short token through the prefix/suffix.
inline std::string mask_secret(const std::string& s) {
    if (s.empty())    return "<not set>";
    if (s.size() < 6) return "************";
    return s.substr(0, 3) + "******" + s.substr(s.size() - 3);
}

// Drains stdin to EOF, trimming trailing newlines. Used when no user message
// was passed on the command line.
inline std::string read_stdin_to_eof() {
    std::string out;
    char buf[4096];
    while (size_t n = std::fread(buf, 1, sizeof(buf), stdin)) {
        out.append(buf, n);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return out;
}

}
}
