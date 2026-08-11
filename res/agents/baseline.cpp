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

#include "client/gcp_auth.hpp"   // Vertex: application default credentials
#include "llm/client.hpp"
#include "llm/gemini.hpp"
#include "llm/mirothinker.hpp"
#include "llm/openai_chat.hpp"
#include "mcp/tool.hpp"
#include "platform/log.hpp"

#include <ctime>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
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
            fp.data      = f.data;   // shares the upload's bytes; no copy (Blob)
            messages[ui].files.push_back(std::move(fp));
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

//------------------------------------------------------------------------------
// Provider menu policy
//------------------------------------------------------------------------------

#ifndef MIROBODY_MOBILE
#  define MIROBODY_MOBILE 0
#endif

// A provider with no credentials is never listed, on any target: it cannot answer
// a single turn, so it is a dead row in the selector that errors on use. This used
// to be mobile-only (kRequireCredentials, MIROBODY_MOBILE) on the argument that a
// desktop operator owns the config and a failing entry is the quickest way to see
// what is missing -- that discovery now comes from the startup log below, at info
// level so it is visible without lowering the log level, which is where a
// misconfiguration belongs rather than in the end user's model picker.
//
// Register `client` under `key` unless `credential` is empty. `credential` is
// whatever makes the provider callable -- an API key for the hosted ones, a base
// URL for a local server that needs none.
void offer(ClientMap& clients, const char* key, const std::string& credential,
           std::shared_ptr<mirobody::llm::Client> client) {
    if (credential.empty()) {
        mirobody::platform::log_info("agent: '%s' not offered (no credential)", key);
        return;
    }
    clients[key] = client;
}

// Build BaselineAgent's provider clients from config. Each provider is keyed by the
// model name the /api/providers selector shows ("Baseline/<provider>") -- always
// slash-free, because agent/provider routing splits the token on "/". Keys / URLs
// are read from the typed Config fields where present, otherwise the shared
// key-value store (which also falls back to environment variables), matching
// the keys the debug CLIs use. An unconfigured provider is skipped by offer()
// above, so what this returns is exactly what can be called.
ClientMap load_clients(const mirobody::Config& cfg) {
    ClientMap clients;

    // The function-call descriptors + executor handed to the OpenAI / Gemini
    // clients so the model can invoke the registered MCP tools locally. All
    // tools, auth-scoped included, on every profile: the embedded mobile build
    // runs its turns as the device owner (user_id 1 -- see run_chat_turn in
    // src/platform/c_api.cpp), so the auth tools genuinely work there too.
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
        offer(clients, "gpt-5-nano", opt.api_key,
              mirobody::llm::make_client<mirobody::llm::OpenAIChatClient>(opt));
    }

    // NVIDIA NIM (build.nvidia.com), OpenAI-compatible, all on the free developer
    // tier so list prices are zero. Every API model id carries a vendor slash
    // while the selector key stays slash-free -- agent/provider routing splits the
    // token on "/". Model ids below were read from GET /v1/models and each was
    // confirmed callable (2026-07-27); the free tier is heavily oversubscribed, so
    // a transient 503 "Worker local total request limit reached" is normal and a
    // retry usually lands.
    {
        struct NvidiaModel {
            const char* key;     // slash-free selector key
            const char* model;   // NIM API model id
            const char* extra;   // extra_body_json, or "" for none
        };
        // NOT offered: NVIDIA's DeepSeek V4 (deepseek-ai/deepseek-v4-flash and
        // -pro). Two strikes, verified 2026-07-27. (1) Reliability: pro never
        // completed a single call all day (40s/120s hangs, 500, 504) and flash
        // intermittently returns an empty turn; the free tier is oversubscribed
        // on exactly these weights. (2) A quirk we would carry forever: both are
        // reasoning models that stream ONLY reasoning_content -- an empty answer
        // -- unless the request carries
        //     {"chat_template_kwargs":{"thinking":false}}
        // (that is what OpenAIChatOptions::extra_body_json exists for, should
        // they come back).
        const NvidiaModel kModels[] = {
            { "glm-5.2",           "z-ai/glm-5.2",                  "" },
            { "gemma-4-31b-it",    "google/gemma-4-31b-it",         "" },
            // A reasoning model too, but a well-behaved one: it streams
            // reasoning_content first and then content, which the client already
            // maps to Thinking then Reply. It does spend a large slice of the
            // token budget thinking before answering.
            { "gpt-oss-120b",      "openai/gpt-oss-120b",           "" },
        };
        for (std::size_t i = 0; i < sizeof(kModels) / sizeof(kModels[0]); ++i) {
            mirobody::llm::OpenAIChatOptions opt;
            opt.api_key         = cfg.nvidia.api_key;
            opt.base_url        = cfg.nvidia.base_url + "/v1";   // client appends /chat/completions
            opt.model           = kModels[i].model;
            opt.input_price     = 0.0;    // free developer tier
            opt.output_price    = 0.0;
            opt.tools_json      = openai_tools;
            opt.tool_executor   = &run_tool_for_user;
            opt.extra_body_json = kModels[i].extra;
            offer(clients, kModels[i].key, opt.api_key,
                  mirobody::llm::make_client<mirobody::llm::OpenAIChatClient>(opt));
        }
    }

    // glm-4.7-flash -- Zhipu GLM (open.bigmodel.cn), OpenAI-compatible. Permanently
    // free with no total cap; the recommended long-term free domestic option.
    // base_url already includes the /api/paas/v4 root, so no "/v1" is appended.
    {
        mirobody::llm::OpenAIChatOptions opt;
        opt.api_key       = cfg.zhipu.api_key;
        opt.base_url      = cfg.zhipu.base_url;   // /api/paas/v4 root; client appends /chat/completions
        opt.model         = "glm-4.7-flash";
        opt.input_price   = 0.0;    // permanently free
        opt.output_price  = 0.0;
        opt.tools_json    = openai_tools;
        opt.tool_executor = &run_tool_for_user;
        offer(clients, "glm-4.7-flash", opt.api_key,
              mirobody::llm::make_client<mirobody::llm::OpenAIChatClient>(opt));
    }

    // Gemini -- AI Studio or Vertex AI, decided by config (see the block below).
    //
    // The two surfaces do NOT serve the same models, so they get their own lists.
    // Offering a row the surface cannot serve is not harmless: it is a live entry
    // in the end user's model picker that 404s when chosen.
    //
    //   AI Studio: gemini-3.6-flash is the current model. gemini-2.5-flash is
    //     closed to new sign-ups -- a freshly issued key 404s on it, on every
    //     platform, not just mobile -- but stays listed on desktop because keys
    //     issued before the cut-off still work. Mobile ships to new users only,
    //     so there it would be a dead row and is left out.
    //
    //   Vertex: gemini-3.5-flash rather than 3.6. 3.6 publishes ONE location,
    //     `global`, so any deployment pinned to a region cannot reach it at all;
    //     3.5 serves `global` plus the `us` / `eu` multi-regions and a handful of
    //     single regions. 2.5 is listed without the desktop-only guard: the
    //     sign-up cut-off is an AI Studio restriction, not a Vertex one.
    //
    // Prices are the STANDARD tier, USD / 1M tokens, since this is the plain
    // (non-batch) generateContent path. Batch runs at half these numbers
    // (2.5: 0.15 / 1.25, 3.6: 0.75 / 3.75) -- not what we bill against here.
    // Vertex charges 3.5 about 10% more away from the `global` endpoint
    // (1.65 / 9.90 against 1.50 / 9.00), so its row is priced from the location.
    struct GeminiModel {
        const char* model;
        double      input_price;
        double      output_price;   // includes thinking tokens
    };
    {
        // Surface selection: Vertex when the deployment names a project AND a
        // location -- the two things its endpoint URL is built from -- and AI
        // Studio otherwise. Both, not either: a URL cannot be built from half of
        // them, and quietly falling back to AI Studio would route a deployment
        // that meant to stay on Vertex (usually for regulatory reasons) to a
        // global consumer endpoint instead. A half-set is reported, not absorbed.
        //
        // The access token is deliberately NOT part of that decision: in a normal
        // deployment nobody configures one. Application Default Credentials finds
        // it (gcp::AccessTokens, the port of google.auth.default() +
        // creds.refresh()), which on GCP means the instance metadata server.
        // Requiring a token here would mean requiring an operator to hand-manage
        // what the platform already provides. Two escape hatches sit ahead of ADC
        // for deployments that do hand one over:
        //
        //   GCP_ACCESS_TOKEN_FILE  a path kept current by something else (a
        //                          projected service-account token, a sidecar).
        //                          Re-read per request, so a rotation lands
        //                          without a restart.
        //   GCP_ACCESS_TOKEN       the token inline. Read once, at startup: a
        //                          process's environment cannot be changed from
        //                          outside, so it only tracks a refresh that
        //                          restarts us, and past its ~1h expiry every
        //                          Gemini turn 401s. Handy for a quick
        //                          `gcloud auth print-access-token` locally.
        //
        // Key names are Google's own wherever Google has one -- GOOGLE_CLOUD_*,
        // GOOGLE_API_KEY, GOOGLE_APPLICATION_CREDENTIALS (read by gcp_auth) -- so
        // a host already set up for gcloud or the Python SDK needs nothing new,
        // and one spelling per setting means a half-migrated deployment cannot end
        // up with the two disagreeing. Earlier revisions also took GCP_PROJECT /
        // GCP_LOCATION; those are gone, and load_config warns when either is still
        // set rather than letting the value fall on the floor. There is no official
        // env var for a raw access token (Google's answer is ADC), so
        // GCP_ACCESS_TOKEN* are our own names -- one pair, shared with the
        // embedding lane through gcp::TokenSource.
        const std::string gcp_token_file = cfg.store.get_str("GCP_ACCESS_TOKEN_FILE");
        const std::string gcp_token      = cfg.store.get_str("GCP_ACCESS_TOKEN");
        const std::string gcp_project    = cfg.store.get_str("GOOGLE_CLOUD_PROJECT");
        const std::string gcp_location   = cfg.store.get_str("GOOGLE_CLOUD_LOCATION");
        const std::string gemini_key     = cfg.store.get_str("GOOGLE_API_KEY", cfg.gemini.api_key);

        // Host overrides, so a deployment can put a proxy / gateway / mock in
        // front of Gemini. GEMINI_BASE_URL is the key the other Gemini callers
        // already honor (the file parser, the embedding client) and it lands on
        // the typed Config field; GEMINI_AI_STUDIO_BASE_URL / GEMINI_VERTEX_BASE_URL
        // are cli/gemini.cpp's per-surface names, kept here so the debug CLI and
        // the server can be pointed at the same host by the same key. Empty
        // leaves the client on its own default, which is what an unset config
        // and the stock GEMINI_BASE_URL default both amount to.
        const std::string ai_studio_base = cfg.store.get_str("GEMINI_AI_STUDIO_BASE_URL",
                                                             cfg.gemini.base_url);
        const std::string vertex_base    = cfg.store.get_str("GEMINI_VERTEX_BASE_URL");

        const bool vertex = !gcp_project.empty() && !gcp_location.empty();

        // Per-model location overrides, read once and consulted per row below.
        // The two Vertex rows NEED this: 3.5 publishes the `us` / `eu` multi-
        // regions and 2.5 the single ones, with no value serving both, so before
        // this map one of the two was always a 404 waiting to be picked. See
        // gcp::vertex_model_location for why the lists are not reconcilable.
        const std::unordered_map<std::string, std::string> model_locations =
            cfg.store.get_dict("GOOGLE_CLOUD_MODEL_LOCATIONS");

        // One token source shared by every Gemini client built below, so the
        // models share a token (and its cache) rather than resolving one each.
        // It stays alive because each client's closure captures the shared_ptr.
        // Built only when it will be used: an AI Studio deployment has no reason
        // to hold one. Resolution order (file, inline, ADC) lives in TokenSource,
        // so this lane and the embedding lane cannot drift apart.
        std::shared_ptr<mirobody::gcp::TokenSource> gcp_tokens;
        if (vertex) {
            gcp_tokens = std::make_shared<mirobody::gcp::TokenSource>(gcp_token_file, gcp_token);
        }

        if (vertex && !gcp_token_file.empty() &&
            mirobody::gcp::read_token_file(gcp_token_file).empty()) {
            mirobody::platform::log_warn(
                "agent: Vertex token file '%s' is missing or empty; Gemini turns will fail "
                "until it is written (it is re-read per request, so no restart is needed)",
                gcp_token_file.c_str());
        }
        if (!vertex && (!gcp_project.empty() || !gcp_location.empty())) {
            mirobody::platform::log_warn(
                "agent: incomplete Vertex config (project=%s location=%s); both are "
                "required, falling back to AI Studio",
                gcp_project.empty()  ? "unset" : "set",
                gcp_location.empty() ? "unset" : "set");
        }
        if (vertex) {
            mirobody::platform::log_info("agent: gemini via Vertex AI (project=%s, location=%s, token=%s)",
                                         gcp_project.c_str(), gcp_location.c_str(),
                                         gcp_tokens->describe());
            // Named individually rather than counted: a model reached at some
            // other location than the one the line above just reported is
            // exactly what someone reading this log after a 404 needs to see.
            for (std::unordered_map<std::string, std::string>::const_iterator
                     it = model_locations.begin(); it != model_locations.end(); ++it) {
                mirobody::platform::log_info("agent:   location override: %s -> %s",
                                             it->first.c_str(), it->second.c_str());
            }
        } else {
            mirobody::platform::log_info("agent: gemini via AI Studio");
        }

        // What makes a row callable differs by surface, so offer() gates on the
        // credential this mode will actually send. On Vertex the project stands
        // in for it: the token is resolved per request (and may be ambient), so
        // there is nothing here to test -- naming a project IS the intent to
        // call Vertex, and a token that never arrives fails the turn with a
        // credential error rather than hiding the whole model list.
        const std::string credential = vertex ? gcp_project : gemini_key;

        // Each surface offers only what it can serve (see the note above the
        // GeminiModel struct). Built here rather than as a static table because
        // the Vertex prices depend on the configured location.
        std::vector<GeminiModel> models;
        if (vertex) {
            // 3.5 is ~10% cheaper on `global`, so its row has to be priced from
            // the location it will actually be REACHED at, not the deployment's.
            const bool global_endpoint =
                mirobody::gcp::vertex_model_location(model_locations, "gemini-3.5-flash",
                                                     gcp_location) == "global";
            const GeminiModel v[] = {
                {"gemini-2.5-flash", 0.30, 2.50},
                {"gemini-3.5-flash", global_endpoint ? 1.50 : 1.65,
                                     global_endpoint ? 9.00 : 9.90},
            };
            models.assign(v, v + sizeof(v) / sizeof(v[0]));
        } else {
            const GeminiModel a[] = {
#if !MIROBODY_MOBILE
                {"gemini-2.5-flash", 0.30, 2.50},
#endif
                {"gemini-3.6-flash", 1.50, 7.50},
            };
            models.assign(a, a + sizeof(a) / sizeof(a[0]));
        }
        for (std::size_t mi = 0; mi < models.size(); ++mi) {
            const GeminiModel& m = models[mi];
            mirobody::llm::GeminiOptions opt;
            opt.mode = vertex ? mirobody::llm::GeminiMode::Vertex
                              : mirobody::llm::GeminiMode::AiStudio;
            if (vertex) {
                // Captured by shared_ptr: the client outlives this scope and the
                // closure runs on a turn's thread long after. Resolved per
                // request, so a rotated token lands without a restart.
                std::shared_ptr<mirobody::gcp::TokenSource> tokens = gcp_tokens;
                opt.access_token_provider = [tokens]() { return tokens->token(); };
                opt.gcp_project     = gcp_project;
                opt.gcp_location    = mirobody::gcp::vertex_model_location(
                                          model_locations, m.model, gcp_location);
                opt.vertex_base_url = vertex_base;
            } else {
                opt.api_key            = gemini_key;
                opt.ai_studio_base_url = ai_studio_base;
            }
            opt.model         = m.model;
            opt.input_price   = m.input_price;
            opt.output_price  = m.output_price;
            opt.tools_json    = gemini_tools;
            opt.tool_executor = &run_tool_for_user;
            offer(clients, m.model, credential,
                  mirobody::llm::make_client<mirobody::llm::GeminiClient>(opt));
        }
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
        offer(clients, "mirothinker-1.7", opt.api_key,
              mirobody::llm::make_client<mirobody::llm::MiroThinkerClient>(opt));
    }

    // Any other OpenAI-compatible provider, declared entirely by config.
    //
    // WHY THIS EXISTS: the blocks above name their models at build time, which is
    // right for a curated free tier but wrong for a BYOK client whose user picks
    // models from the provider's live catalog. Those turns used to fall back to the
    // client's own direct SSE transport -- same upstream call, but outside the agent
    // pipeline, so no tools: no render_chart, no ask_user, no family_health. The
    // gap was never "BYOK cannot call tools", it was "the core had not been told
    // about this provider", and a client cannot fix it by parsing tool_calls itself
    // because the tools live here.
    //
    // Config, per provider id (the client injects these through mirobody_set_config
    // and then reloads):
    //
    //   <ID>_API_KEY    required; absent => the provider is not offered at all
    //   <ID>_BASE_URL   required; the OpenAI-compatible root, /chat/completions is
    //                   appended by the client. No default: guessing a URL for a
    //                   provider we have not tested is how you ship a broken entry.
    //   <ID>_MODELS     comma-separated model ids to register
    //
    // The selector key is the model id's LAST path segment, matching how the
    // NVIDIA block keys "z-ai/glm-5.2" as "glm-5.2" and how the clients resolve a
    // model to a provider. Two providers offering the same short name collide, and
    // last registration wins -- the same property the curated blocks already have.
    {
        const char* const kCompatIds[] = { "OPENROUTER", "GROQ", "CEREBRAS" };
        for (std::size_t i = 0; i < sizeof(kCompatIds) / sizeof(kCompatIds[0]); ++i) {
            const std::string id  = kCompatIds[i];
            const std::string key = cfg.store.get_str(id + "_API_KEY");
            const std::string base = cfg.store.get_str(id + "_BASE_URL");
            const std::string models = cfg.store.get_str(id + "_MODELS");
            if (key.empty() || base.empty() || models.empty()) continue;

            std::size_t from = 0;
            while (from <= models.size()) {
                const std::size_t comma = models.find(',', from);
                const std::size_t end = (comma == std::string::npos) ? models.size() : comma;
                std::string model = models.substr(from, end - from);
                from = end + 1;

                // Tolerate spaces around the separators; skip empties rather than
                // registering a client for "".
                while (!model.empty() && model[0] == ' ') model.erase(0, 1);
                while (!model.empty() && model[model.size() - 1] == ' ') model.erase(model.size() - 1);
                if (model.empty()) { if (comma == std::string::npos) break; continue; }

                const std::size_t slash = model.rfind('/');
                const std::string selector =
                    (slash == std::string::npos) ? model : model.substr(slash + 1);

                mirobody::llm::OpenAIChatOptions opt;
                opt.api_key       = key;
                opt.base_url      = base;
                opt.model         = model;
                // BYOK: the user's own account is billed, and we have no price
                // table for an arbitrary catalog entry. Reporting zero would be a
                // lie the cost summary repeats; reporting nothing is honest.
                opt.input_price   = 0.0;
                opt.output_price  = 0.0;
                opt.tools_json    = openai_tools;
                opt.tool_executor = &run_tool_for_user;
                // c_str() is safe: offer() inserts into a std::string-keyed map.
                offer(clients, selector.c_str(), key,
                      mirobody::llm::make_client<mirobody::llm::OpenAIChatClient>(opt));

                if (comma == std::string::npos) break;
            }
        }
    }

    // gemma-4-e2b -- the same model the native apps run on-device (Gemma 4 E2B),
    // served here over a local OpenAI-compatible endpoint (Ollama / llama.cpp /
    // vLLM). Lets the mobile clients chat with a server-hosted copy -- handy for
    // checking the model's behaviour without the ~2.5 GB on-device download.
    // Registered only when a base URL is configured, so it never shows as a broken
    // provider by default. Its credential is that base URL, not a key -- a local
    // server usually wants none -- so that is what gates it on every target.
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
            offer(clients, "gemma-4-e2b", base,
                  mirobody::llm::make_client<mirobody::llm::OpenAIChatClient>(opt));
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
