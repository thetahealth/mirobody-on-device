// Standalone MCP tool-registry inspector / invoker.
//
// Exercises the compile-time tool registry (src/mcp/tool.hpp) end-to-end
// without starting the server: list the registered tools, dump the JSON
// Schemas generated from their Param tables, emit the OpenAI / Gemini
// function-call descriptors, or invoke a tool and print its result. The
// registry is populated by the same res/mcp_tools/*.cpp objects the server links
// (wired in via $<TARGET_OBJECTS:mcp_tools>), so what this prints is exactly
// what the /mcp endpoint would serve.
//
//   mcp list                        List tool names, auth flag, description.
//   mcp schema [name]               Dump tools/list JSON (all tools, or one).
//   mcp functions [openai|gemini]   Dump LLM function-call descriptors.
//   mcp call <name> ['<json-args>'] Invoke a tool; args JSON from arg or piped stdin.
//       --user <id>                 Identity for an auth-flagged tool.
//
// Examples:
//   mcp list
//   mcp schema echo
//   mcp call echo '{"text":"hi"}'
//   echo '{"text":"hi"}' | mcp call echo
//   mcp call whoami --user 123
//
// stdout carries the data (JSON); logs (including the tools' load-time
// registration lines) go to stderr, so piping stdout stays clean.

#include "mcp/tool.hpp"
#include "platform/log.hpp"

#include "compat/cxx11.hpp"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define MCP_ISATTY(fd) _isatty(fd)
#define MCP_FILENO(f)  _fileno(f)
#else
#include <unistd.h>
#define MCP_ISATTY(fd) isatty(fd)
#define MCP_FILENO(f)  fileno(f)
#endif

using mirobody::mcp::Args;
using mirobody::mcp::Result;
using mirobody::mcp::Tool;
using mirobody::mcp::UserInfo;
using mirobody::mcp::registry;

namespace {

//------------------------------------------------------------------------------

void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s <command> [args]\n"
        "\n"
        "Inspect and invoke the in-process MCP tool registry.\n"
        "\n"
        "Commands:\n"
        "  list                        List tool names, auth flag, description.\n"
        "  schema [name]               Dump tools/list JSON (all tools, or one).\n"
        "  functions [openai|gemini]   Dump LLM function-call descriptors.\n"
        "  call <name> ['<json-args>'] Invoke a tool; args JSON from arg or piped stdin.\n"
        "      --user <id>             Identity for an auth-flagged tool.\n"
        "\n"
        "Examples:\n"
        "  %s list\n"
        "  %s call echo '{\"text\":\"hi\"}'\n"
        "  %s call whoami --user 123\n",
        prog, prog, prog, prog);
}

//------------------------------------------------------------------------------

// Pretty-print a JSON string to stdout. Returns 1 on parse failure (should not
// happen for registry-produced JSON).
int print_pretty(const std::string& json) {
    rapidjson::Document d;
    if (d.Parse(json.c_str()).HasParseError()) {
        std::fprintf(stderr, "error: produced invalid JSON\n");
        return 1;
    }
    rapidjson::StringBuffer buf;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> w(buf);
    d.Accept(w);
    std::fwrite(buf.GetString(), 1, buf.GetSize(), stdout);
    std::fputc('\n', stdout);
    return 0;
}

std::string read_stdin() {
    std::string out;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), stdin)) > 0) out.append(buf, n);
    return out;
}

// True when stdin is a pipe / file (not an interactive terminal). Used to
// decide whether `call` should read arguments from stdin: reading a piped
// payload is intended, but blocking on keyboard input when none was provided
// just hangs the CLI.
bool stdin_is_piped() {
    return MCP_ISATTY(MCP_FILENO(stdin)) == 0;
}

bool is_blank(const std::string& s) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
    }
    return true;
}

//------------------------------------------------------------------------------

int cmd_list() {
    const std::vector<std::string> names = registry().names();
    if (names.empty()) {
        std::fprintf(stderr, "(no tools registered)\n");
        return 0;
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        const Tool* t = registry().find(names[i]);
        if (!t) continue;
        std::printf("%-24s %s  %s\n",
                    t->name.c_str(),
                    t->auth ? "[auth]" : "      ",
                    t->description.c_str());
    }
    return 0;
}

int cmd_schema(const char* name) {
    const std::string all = registry().tools_list_json();
    if (!name) return print_pretty(all);

    // Filter the tools/list array down to the named tool.
    rapidjson::Document d;
    d.Parse(all.c_str());
    if (!d.IsArray()) return 1;
    for (rapidjson::SizeType i = 0; i < d.Size(); ++i) {
        if (d[i].HasMember("name") && d[i]["name"].IsString() &&
            std::string(d[i]["name"].GetString()) == name) {
            rapidjson::StringBuffer buf;
            rapidjson::PrettyWriter<rapidjson::StringBuffer> w(buf);
            d[i].Accept(w);
            std::fwrite(buf.GetString(), 1, buf.GetSize(), stdout);
            std::fputc('\n', stdout);
            return 0;
        }
    }
    std::fprintf(stderr, "error: no such tool '%s'\n", name);
    return 1;
}

int cmd_functions(const char* style) {
    return print_pretty(registry().functions_json(style ? style : ""));
}

int cmd_call(const std::string& name, const std::string& args_json, const std::string& user_id) {
    rapidjson::Document args;
    if (is_blank(args_json)) {
        args.SetObject();
    } else if (args.Parse(args_json.c_str()).HasParseError() || !args.IsObject()) {
        std::fprintf(stderr, "error: arguments must be a JSON object\n");
        return 2;
    }

    UserInfo user;
    if (!user_id.empty()) {
        user.user_id = std::strtoll(user_id.c_str(), nullptr, 10);
    }

    const Result r = registry().call(name, args, user);
    if (!r.success) {
        std::fprintf(stderr, "error: %s\n", r.json.c_str());
        return 1;
    }
    return print_pretty(r.json);
}

}   // namespace

//------------------------------------------------------------------------------

int main(int argc, char** argv) {
    // Keep runtime info/warn chatter off stderr. The tools' load-time
    // registration lines still print -- they log during static init, before
    // main() runs -- but they go to stderr, so piped stdout stays clean.
    mirobody::platform::set_log_level(mirobody::platform::LogLevel::Warn);

    if (argc < 2) { print_usage(argv[0]); return 2; }

    const std::string cmd = argv[1];
    if (cmd == "-h" || cmd == "--help") { print_usage(argv[0]); return 0; }

    // Collect --user and gather the remaining positionals (argv[2..]).
    std::string user_id;
    std::vector<std::string> pos;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--user") {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for --user\n"); return 2; }
            user_id = argv[++i];
        } else {
            pos.push_back(argv[i]);
        }
    }

    if (cmd == "list") {
        return cmd_list();
    }
    if (cmd == "schema") {
        return cmd_schema(pos.empty() ? nullptr : pos[0].c_str());
    }
    if (cmd == "functions") {
        return cmd_functions(pos.empty() ? nullptr : pos[0].c_str());
    }
    if (cmd == "call") {
        if (pos.empty()) { std::fprintf(stderr, "error: call requires a tool name\n"); return 2; }
        const std::string name = pos[0];
        // Args from the next positional, else piped stdin. With no inline args
        // and an interactive terminal, default to empty rather than blocking on
        // keyboard input.
        std::string args_json;
        if (pos.size() > 1)        args_json = pos[1];
        else if (stdin_is_piped()) args_json = read_stdin();
        return cmd_call(name, args_json, user_id);
    }

    std::fprintf(stderr, "unknown command: %s\n", argv[1]);
    print_usage(argv[0]);
    return 2;
}
