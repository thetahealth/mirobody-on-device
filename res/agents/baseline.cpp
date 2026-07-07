// BaselineAgent -- the minimal health-assistant agent.
//
// The C++ analog of pub/agents/base_agent.py: no agent loop or middleware, it
// just builds a system prompt, picks an LLM client by provider name, and
// streams the provider's events straight through. This file is globbed into
// the build from res/agents/ and self-registers via MIROBODY_REGISTER_AGENT.
//
// Scope note: the Python BaseAgent also forwards a filtered MCP tool list to
// the client for local function-calling. The C++ llm::Client::ainvoke does not
// take a tool list yet (tool use is provider-native, configured on the client),
// so allowed/disallowed filtering is accepted and recorded but not yet handed
// to the provider. Prompt templating is a fixed inline string rather than the
// Jinja template the Python version renders.

#include "chat/agent.hpp"

#include "llm/client.hpp"
#include "llm/gemini.hpp"
#include "llm/mirothinker.hpp"
#include "llm/openai_chat.hpp"
#include "mcp/tool.hpp"
#include "platform/log.hpp"

#include <ctime>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace mirobody::chat;

const char* const kAgentName = "Baseline";

//------------------------------------------------------------------------------

// Keep only the last `max_user_turns` user turns and everything after the
// earliest of them -- the port of _trim_to_recent_user_turns.
std::vector<mirobody::llm::ChatMessage>
trim_to_recent_user_turns(const std::vector<mirobody::llm::ChatMessage>& messages,
                          int max_user_turns) {
    int seen = 0;
    for (std::size_t i = messages.size(); i-- > 0; ) {
        if (messages[i].role == "user") {
            ++seen;
            if (seen >= max_user_turns) {
                if (i == 0) return messages;
                return std::vector<mirobody::llm::ChatMessage>(messages.begin() + i, messages.end());
            }
        }
    }
    return messages;
}

std::string build_system_prompt(const std::string& language, const std::string& timezone,
                                bool file_tools, std::int64_t subject_user_id) {
    std::time_t now = std::time(nullptr);
    char when[64] = {0};
    std::tm tmv;

    // Always report UTC. The client sends its IANA timezone (e.g.
    // "America/New_York"), but C++11 has no portable way to render `now` in an
    // arbitrary zone, and tagging a server-local timestamp with the client's
    // zone would misstate the time on any host not in that zone (every hosted
    // *.mirobody.ai deployment). UTC is unambiguous; when the client's zone is
    // known we name it separately so the model can localize wherever the server
    // runs.
#ifdef _WIN32
    gmtime_s(&tmv, &now);
#else
    gmtime_r(&now, &tmv);
#endif
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tmv);

    std::string prompt =
        "You are Theta, a careful and concise health assistant.\n";
    if (!language.empty())
        prompt += "Respond in " + language + ".\n";
    prompt += "Current time: " + std::string(when) + " UTC.\n";
    if (!timezone.empty())
        prompt += "The user's timezone is " + timezone + ".\n";

    // Uploaded-files guidance, for signed-in users only (the tools require
    // auth). Text is extracted from every upload at upload time, so even a
    // file format the model can't read directly -- or an image from an
    // earlier turn whose bytes are no longer attached -- is reachable as
    // text through list_files / read_file. Without this the model gives up
    // ("I cannot process images") instead of using the tools.
    if (file_tools) {
        prompt +=
            "The user may have uploaded files (images, PDFs, lab reports). "
            "Text is extracted from every upload and stored server-side. "
            "To read an uploaded file -- including images and other formats "
            "you cannot view directly -- first call the list_files tool to "
            "see the uploads, then call read_file with a file_key to get "
            "that file's text content. Do this instead of replying that you "
            "cannot process an image or file.\n";

        // Health records (own + shared care-circle members) are reachable through
        // the family_health tool. Always advertise it for signed-in users so the
        // model fetches data instead of guessing.
        prompt +=
            "To answer questions about health records -- the user's own or a "
            "care-circle member's who shared theirs -- call the family_health "
            "tool (omit `member` or use \"me\" for the user; pass a name or "
            "numeric id for a member). Only members who shared their data are "
            "reachable.\n";
        if (subject_user_id > 0) {
            const std::string id = std::to_string(subject_user_id);
            prompt +=
                "The user is currently focused on care-circle member id " + id +
                " (a member who shared their health data); for health questions "
                "call family_health with member=\"" + id + "\" unless they clearly "
                "mean someone else.\n";
        }
    }

    return prompt;
}

// Attach this turn's uploaded files to the latest user message. Files that carry
// bytes are inlined as llm::FilePart, so a provider that supports it renders the
// content directly (e.g. Gemini inlineData) -- the only way the model actually
// "sees" an image, since it can't fetch a URL. Reference-only files (no bytes)
// fall back to a URL line in the message text. A short text note names every
// file either way. Returns the number inlined (for logging).
std::size_t attach_files_to_turn(std::vector<mirobody::llm::ChatMessage>& messages,
                                 const std::vector<AgentFile>& files) {
    if (files.empty() || messages.empty()) return 0;

    // The files rode on the latest user turn; attach them there.
    std::size_t ui = messages.size();
    for (std::size_t i = messages.size(); i-- > 0; ) {
        if (messages[i].role == "user") { ui = i; break; }
    }
    if (ui >= messages.size()) return 0;

    std::size_t inlined = 0;
    std::string note;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const AgentFile& f = files[i];
        note += "\n- " + f.filename;
        if (!f.mime_type.empty()) note += " (" + f.mime_type + ")";
        // The handle for the read_file tool. Also the file's only trace on
        // later turns: conversation memory replays text, not bytes, so a
        // follow-up about this file reads its extracted text through the
        // tools rather than seeing the upload again.
        if (!f.file_key.empty()) note += ", file_key: " + f.file_key;
        if (!f.data.empty()) {
            mirobody::llm::FilePart fp;
            fp.mime_type = f.mime_type;
            fp.data      = f.data;
            messages[ui].files.push_back(fp);
            ++inlined;
        } else if (!f.url.empty()) {
            note += ": " + f.url;   // reference-only: best we can do is the link
        }
    }
    if (!note.empty()) {
        messages[ui].content += "\n\n[Attached files]" + note +
            "\n(Each file's extracted text is available: call the read_file "
            "tool with its file_key, or list_files to see every upload.)";
    }
    return inlined;
}

//------------------------------------------------------------------------------

class BaselineAgent : public Agent {
public:
    explicit BaselineAgent(const AgentRequest& req)
        : default_provider_("gemini-2.5-flash")
        , user_message_threshold_(10) {
        (void)req;   // per-request options (user/tools) are read in generate_response
    }

    void generate_response(const AgentRequest& req,
                           const mirobody::llm::EventHandler& on_event) override {
        if (req.messages.empty()) {
            mirobody::llm::Event e;
            e.type = mirobody::llm::EventType::Error;
            e.content = "Empty message.";
            on_event(e);
            return;
        }

        std::vector<mirobody::llm::ChatMessage> messages =
            trim_to_recent_user_turns(req.messages, user_message_threshold_);

        // Forward uploaded files on the latest user turn: inline the bytes where
        // we have them (Gemini inlineData), URL otherwise. See attach_files_to_turn.
        if (!req.files.empty()) {
            const std::size_t inlined = attach_files_to_turn(messages, req.files);
            mirobody::platform::log_debug(
                "chat[4/agent]: forwarded %lu file(s) to the model (%lu inlined, %lu by URL)",
                (unsigned long)req.files.size(), (unsigned long)inlined,
                (unsigned long)(req.files.size() - inlined));
        }

        // Detect the response language: a CJK / Arabic / Hebrew / Cyrillic script
        // in the user's message wins outright; Latin script tells the model to
        // mirror the user. The client's Accept-Language is the fallback -- it
        // disambiguates a Latin-script message and gives a text-less (upload-only)
        // turn a language instead of leaving it to the model.
        const std::string language = detect_language(req.question, req.accept_language);

        // The list_files / read_file tools require an authenticated caller, so
        // only a signed-in user's prompt advertises them.
        const std::string prompt =
            build_system_prompt(language, req.timezone, req.user_id > 0, req.subject_user_id);
        mirobody::platform::log_debug("chat[4/agent]: system prompt:\n%s", prompt.c_str());

        // Resolve the client: requested provider, then the agent default.
        const std::string provider = req.provider.empty() ? default_provider_ : req.provider;
        std::shared_ptr<mirobody::llm::Client> client =
            agent_registry().client(kAgentName, provider);
        if (!client && provider != default_provider_) {
            client = agent_registry().client(kAgentName, default_provider_);
        }
        if (!client) {
            mirobody::llm::Event e;
            e.type = mirobody::llm::EventType::Error;
            e.content = "provider " + provider + " not found";
            on_event(e);
            return;
        }

        // Thread the caller's identity so locally-executed tools run as the
        // authenticated user (the gpt/gemini clients forward it to their tool
        // executor; MiroThinker ignores it — it runs tools provider-side).
        mirobody::llm::UserContext user;
        user.user_id         = req.user_id;
        user.session_id      = req.session_id;
        user.subject_user_id = req.subject_user_id;   // authorized "currently for" member (read-only)
        user.cache      = req.cache;     // forwarded to the tool executor below
        user.storage    = req.storage;
        user.memory     = req.memory;
        user.db         = req.db;

        client->ainvoke(messages, prompt, on_event, user);
    }

private:
    std::string default_provider_;
    int         user_message_threshold_;
};

//------------------------------------------------------------------------------

// The tool_executor handed to the OpenAI / Gemini clients — the local-execution
// analog of MiroThinker's provider-native MCP (which dispatches over
// MCP_PUBLIC_URL). It adapts the provider-neutral llm::UserContext threaded
// through ainvoke into the mcp::UserInfo the registry expects, so auth-scoped
// tools see the real caller, then runs the named tool (see mcp::run_mcp_tool).
std::string run_tool_for_user(const std::string& name, const std::string& args_json,
                              const mirobody::llm::UserContext& uc) {
    mirobody::mcp::UserInfo user;
    user.user_id         = uc.user_id;   // both are the raw row id as int64
    user.session_id      = uc.session_id;
    user.subject_user_id = uc.subject_user_id;   // read-only care-circle subject, if any
    // Map the request's borrowed services onto the tool context so file-backed
    // tools reach the per-user index + object store, the memory tools reach the
    // long-term store, and the chat-history tools reach the relational store.
    const mirobody::mcp::ToolContext ctx(uc.cache, uc.storage, uc.db, uc.memory);
    return mirobody::mcp::run_mcp_tool(name, args_json, user, ctx);
}

// Build BaselineAgent's provider clients from config. Each provider is keyed by the
// model name the /api/providers selector shows ("Baseline/<provider>"). Keys / URLs
// are read from the typed Config fields where present, otherwise the shared
// key-value store (which also falls back to environment variables), matching
// the keys the debug CLIs use. All three are registered unconditionally so they
// appear in the selector; a provider whose credentials are unset simply errors
// at call time.
ClientMap load_clients(const mirobody::Config& cfg) {
    ClientMap clients;

    // The function-call descriptors + executor handed to the OpenAI / Gemini
    // clients so the model can invoke the registered MCP tools locally.
    const std::string openai_tools = mirobody::mcp::registry().functions_json("openai");
    const std::string gemini_tools = mirobody::mcp::registry().functions_json("gemini");

    // Per-model pricing in USD per 1,000,000 tokens, used to fill the
    // CostStatistics.total_cost the clients report (cost = (in*input_price +
    // (thought+out)*output_price) / 1e6). These are provider list prices; keep
    // them in sync with the pinned model above when pricing or the model changes.

    // gpt-5-nano -- OpenAI-compatible Chat Completions.
    {
        mirobody::llm::OpenAIChatOptions opt;
        opt.api_key  = cfg.openai.api_key;
        opt.base_url = cfg.openai.base_url + "/v1";   // client appends /chat/completions
        opt.model    = "gpt-5-nano";
        opt.input_price    = 0.05;   // USD / 1M input tokens
        opt.output_price   = 0.40;   // USD / 1M output tokens
        opt.tools_json     = openai_tools;
        opt.tool_executor  = &run_tool_for_user;
        clients["gpt-5-nano"] = mirobody::llm::make_client<mirobody::llm::OpenAIChatClient>(opt);
    }

    // gemini-2.5-flash -- Google AI Studio.
    {
        mirobody::llm::GeminiOptions opt;
        opt.api_key = cfg.store.get_str("GOOGLE_API_KEY", cfg.gemini.api_key);
        opt.model   = "gemini-2.5-flash";
        opt.input_price   = 0.30;   // USD / 1M input tokens
        opt.output_price  = 2.50;   // USD / 1M output tokens (incl. thinking tokens)
        opt.tools_json    = gemini_tools;
        opt.tool_executor = &run_tool_for_user;
        clients["gemini-2.5-flash"] = mirobody::llm::make_client<mirobody::llm::GeminiClient>(opt);
    }

    // mirothinker-1.7 -- MiroMind OpenAI-compatible endpoint (provider-native
    // MCP via MCP_PUBLIC_URL).
    {
        mirobody::llm::MiroThinkerOptions opt;
        opt.api_key = cfg.store.get_str("MIROTHINKER_API_KEY");
        opt.mcp_url = cfg.store.get_str("MCP_PUBLIC_URL");
        const std::string base = cfg.store.get_str("MIROTHINKER_BASE_URL");
        if (!base.empty()) opt.base_url = base;
        const std::string model = cfg.store.get_str("MIROTHINKER_MODEL");
        if (!model.empty()) opt.model = model;
        clients["mirothinker-1.7"] = mirobody::llm::make_client<mirobody::llm::MiroThinkerClient>(opt);
    }

    // gemma-4-e2b -- the same model the native apps run on-device (Gemma 4 E2B),
    // served here over a local OpenAI-compatible endpoint (Ollama / llama.cpp /
    // vLLM). Lets the mobile clients chat with a server-hosted copy -- handy for
    // checking the model's behaviour without the ~2.5 GB on-device download.
    // Registered only when a base URL is configured, so it never shows as a broken
    // provider by default (the built-ins above are always-on and error at call).
    {
        const std::string base = cfg.store.get_str("GEMMA_CHAT_BASE_URL",
                                     cfg.store.get_str("OLLAMA_BASE_URL", ""));
        if (!base.empty()) {
            mirobody::llm::OpenAIChatOptions opt;
            opt.api_key       = cfg.store.get_str("GEMMA_CHAT_API_KEY");  // usually empty for a local server
            opt.base_url      = base;   // OpenAI-compatible /v1 root; client appends /chat/completions
            opt.model         = cfg.store.get_str("GEMMA_CHAT_MODEL", "gemma-4-e2b");
            opt.input_price   = 0.0;    // local inference -- no per-token cost
            opt.output_price  = 0.0;
            opt.tools_json    = openai_tools;     // tool use depends on the serving stack
            opt.tool_executor = &run_tool_for_user;
            clients["gemma-4-e2b"] = mirobody::llm::make_client<mirobody::llm::OpenAIChatClient>(opt);
        }
    }

    return clients;
}

const AgentRegistration kBaselineAgent = {
    kAgentName,
    true,                                                    // public
    [](const AgentRequest& req) -> std::unique_ptr<Agent> {
        return std::unique_ptr<Agent>(new BaselineAgent(req));
    },
    &load_clients,
};

}   // namespace

MIROBODY_REGISTER_AGENT(kBaselineAgent);
