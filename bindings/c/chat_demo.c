// Runnable C example for mirobody_chat — a serverless LLM + MCP chat turn.
//
// Unlike the other bindings (which exercise the server lifecycle:
// mirobody_start -> GET / -> mirobody_stop), this one calls mirobody_chat
// directly. No server is started: mirobody_chat loads the process-wide config
// automatically on first use (MIROBODY_CONFIG env -> ./config.yml -> built-in
// defaults) and streams one assistant turn back through a callback.
//
// Usage:  chat_demo "your message" [provider]
//
//   message   the user prompt (required)
//   provider  an "Agent/model" pair as listed by mirobody_get_providers; omit/""
//             => the default agent and its default model. e.g.
//             "Base/gemini-2.5-flash" to use Google AI Studio instead.
//
// The LLM credentials come from the config (OPENAI_API_KEY / GOOGLE_API_KEY,
// etc.). With none set, the turn still runs end-to-end and the provider's
// "missing key" failure arrives as an "error" event — which this demo prints,
// showing the callback path works either way.
//
// Build (Windows, from the repo root, in a VS Developer prompt):
//   cl /nologo /I src bindings\c\chat_demo.c /Fe:build\chat_demo.exe /link build\mirobody.lib
//   build\chat_demo.exe "What is a healthy resting heart rate?"
//
// Build (Linux/macOS):
//   cc -I src bindings/c/chat_demo.c -Lbuild-shared -lmirobody -o build-shared/chat_demo
//   ./build-shared/chat_demo "What is a healthy resting heart rate?"
//
// Run from the repo root so the default config's sql_dir/config.yml resolve,
// with the native dependency dir on PATH so the DLL's transitive deps load.

#include "mirobody.h"

#include <stdio.h>
#include <string.h>

// Context threaded through every on_event call via the user_data pointer. This
// is how the C callback recovers its state without globals — whatever you pass
// as the last argument to mirobody_chat comes straight back here.
typedef struct {
    int    events;          // total events seen
    int    saw_error;       // set if the provider reported an error
    size_t reply_len;       // running length of the assembled answer
    char   reply[8192];     // the coalesced "reply" text
} ChatCtx;

// Streaming callback. Fires once per event on mirobody_chat's calling thread.
// event_type is a stable lowercase tag; content is the UTF-8 payload. Return
// non-zero to keep streaming, 0 to abort the turn early.
static int on_event(const char* event_type, const char* content, void* user_data) {
    ChatCtx* ctx = (ChatCtx*)user_data;
    ctx->events++;

    if (strcmp(event_type, "reply") == 0) {
        // Accumulate the answer, and also echo it live so you see it stream.
        size_t n = strlen(content);
        if (ctx->reply_len + n < sizeof(ctx->reply)) {
            memcpy(ctx->reply + ctx->reply_len, content, n);
            ctx->reply_len += n;
            ctx->reply[ctx->reply_len] = '\0';
        }
        fputs(content, stdout);
        fflush(stdout);
    } else if (strcmp(event_type, "thinking") == 0) {
        fprintf(stderr, "[thinking] %s\n", content);
    } else if (strcmp(event_type, "queryTitle") == 0) {
        fprintf(stderr, "[tool call] %s\n", content);
    } else if (strcmp(event_type, "queryArguments") == 0) {
        fprintf(stderr, "[tool args] %s\n", content);
    } else if (strcmp(event_type, "queryDetail") == 0) {
        fprintf(stderr, "[tool result] %s\n", content);
    } else if (strcmp(event_type, "costStatistics") == 0) {
        fprintf(stderr, "[usage] (turn complete)\n");
    } else if (strcmp(event_type, "error") == 0) {
        ctx->saw_error = 1;
        fprintf(stderr, "[error] %s\n", content);
    }

    return 1;   // keep streaming (return 0 here to cancel mid-turn)
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s \"message\" [agent] [provider]\n", argv[0]);
        return 2;
    }

    const char* message  = argv[1];
    const char* provider = (argc > 2) ? argv[2] : "";   // "Agent/model"; "" => default

    // Discover what we can chat with — each line is a valid `provider` value to
    // hand straight to mirobody_chat below.
    const char* providers = mirobody_get_providers();
    fprintf(stderr, "available providers:\n%s\n\n", providers ? providers : "(none)");

    ChatCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    // user_id 0 => anonymous turn (auth-scoped MCP tools see no user).
    int rc = mirobody_chat(provider, message, /*user_id=*/0, on_event, &ctx);

    fputc('\n', stdout);
    fprintf(stderr, "--- %d event(s), rc=%d%s ---\n",
            ctx.events, rc, ctx.saw_error ? ", provider error" : "");

    // rc != 0 means the call itself failed (bad args / unknown agent / aborted
    // / client init). A provider error is delivered as an event, not via rc.
    return rc == 0 ? 0 : 1;
}
