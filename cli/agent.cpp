// Standalone driver for the agent framework (src/chat/agent.hpp).
//
// Lists the registered agents (res/agents/*.cpp self-register via
// MIROBODY_REGISTER_AGENT) or runs one end to end: load config, build each
// agent's provider clients, construct the named agent, and stream its events.
//
//   agent list
//   agent run Base "What's my blood pressure trend?"
//   agent run Base --provider openai --user u123 "hi"
//   echo "hi" | agent run Base
//
// Config precedence matches the other CLIs: --flag > env > YAML > default.
// Running an agent hits the real provider, so a working OPENAI_API_KEY (and
// the rest of the upstream config) is required for `run`.

#include "chat/agent.hpp"
#include "client/http_client.hpp"
#include "config/config.hpp"
#include "platform/log.hpp"
#include "event_printer.hpp"

#include "compat/cxx11.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using mirobody::chat::AgentRequest;
using mirobody::chat::agent_registry;

namespace {

void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s <command> [options]\n"
        "\n"
        "Inspect and run the in-process agent registry.\n"
        "\n"
        "Commands:\n"
        "  list                       List registered agents (name, visibility).\n"
        "  run <name> [\"message\"]      Run an agent and stream its events.\n"
        "      --config <path>        Local YAML to load.\n"
        "      --provider <name>      LLM provider (default: the agent's default).\n"
        "      --user <id>            Caller user id.\n"
        "      --raw                  One TSV line per event.\n"
        "      --no-color             Disable ANSI escapes.\n"
        "\n"
        "Examples:\n"
        "  %s list\n"
        "  %s run Base \"What's my blood pressure trend?\"\n",
        prog, prog, prog);
}

int cmd_list() {
    const std::vector<std::string> names = agent_registry().names();
    if (names.empty()) {
        std::fprintf(stderr, "(no agents registered)\n");
        return 0;
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        const mirobody::chat::AgentRegistration* reg = agent_registry().find(names[i]);
        if (!reg) continue;
        std::printf("%-20s %s\n", reg->name.c_str(), reg->is_public ? "public" : "private");
    }
    return 0;
}

}   // namespace

//------------------------------------------------------------------------------

int main(int argc, char** argv) {
    if (argc < 2) { print_usage(argv[0]); return 2; }

    const std::string cmd = argv[1];
    if (cmd == "-h" || cmd == "--help") { print_usage(argv[0]); return 0; }

    if (cmd == "list") {
        mirobody::platform::set_log_level(mirobody::platform::LogLevel::Warn);
        return cmd_list();
    }

    if (cmd != "run") {
        std::fprintf(stderr, "unknown command: %s\n", argv[1]);
        print_usage(argv[0]);
        return 2;
    }

    //--------------------------------------------------------------------------
    // run <name> [options] ["message"]

    mirobody::optional<std::string> config_path;
    std::string agent_name, provider, user_id, message;
    bool raw_mode = false, color = true;

    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char* what) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", what); std::exit(2); }
            return argv[++i];
        };
        if      (a == "--config")   { config_path = need("--config"); }
        else if (a == "--provider") { provider = need("--provider"); }
        else if (a == "--user")     { user_id  = need("--user"); }
        else if (a == "--raw")      { raw_mode = true; color = false; }
        else if (a == "--no-color") { color = false; }
        else if (!a.empty() && a.front() == '-') {
            std::fprintf(stderr, "unknown option: %.*s\n", static_cast<int>(a.size()), a.data());
            return 2;
        }
        else if (agent_name.empty()) { agent_name = std::string(a); }
        else                         { message = std::string(a); }
    }

    if (agent_name.empty()) {
        std::fprintf(stderr, "error: run requires an agent name (try `%s list`)\n", argv[0]);
        return 2;
    }
    if (message.empty() && !mirobody::tools::stdin_is_tty()) {
        message = mirobody::tools::read_stdin_to_eof();
    }
    if (message.empty()) {
        std::fprintf(stderr, "error: no message - pass one as an argument or pipe via stdin\n");
        return 2;
    }

    //--------------------------------------------------------------------------

    mirobody::tools::prepare_windows_console();
    mirobody::client::HttpClient::global_init();

    const mirobody::Config cfg = mirobody::load_config(config_path);
    agent_registry().load_clients(cfg);

    AgentRequest req;
    req.question = message;
    req.messages.push_back({"user", message});
    req.provider = provider;
    req.user_id  = std::strtoll(user_id.c_str(), nullptr, 10);   // --user is a numeric row id

    std::unique_ptr<mirobody::chat::Agent> agent = agent_registry().create(agent_name, req);
    if (!agent) {
        std::fprintf(stderr, "error: no such agent '%s' (try `%s list`)\n",
                     agent_name.c_str(), argv[0]);
        mirobody::client::HttpClient::global_cleanup();
        return 2;
    }

    mirobody::tools::EventPrinter printer{raw_mode, color};
    const auto t0 = std::chrono::steady_clock::now();
    agent->generate_response(req, std::ref(printer));
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    if (!raw_mode) std::fputc('\n', stdout);
    mirobody::platform::log_info("agent: '%s' complete in %.2fs", agent_name.c_str(), elapsed);

    mirobody::client::HttpClient::global_cleanup();
    return 0;
}
