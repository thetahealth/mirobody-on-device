#pragma once

// AskBroker -- the rendezvous a tool uses to put a question to the user and wait
// for their answer, mid-turn.
//
// WHY MID-TURN AT ALL: the alternative is for the model to end its turn with a
// question and treat the reply as a fresh user message. That works, and it is what
// happens wherever this tool is not registered -- but the model then cannot ask and
// KEEP WORKING: one piece of work becomes three turns, each re-reading the whole
// conversation. Blocking inside the tool call keeps it one turn, with the answer
// arriving where the model expects it: as the tool's result.
//
// WHY THIS IS FREE ON THE PROVIDER SIDE: the agent loop runs here, not there. A
// round of tool calling is a COMPLETED HTTP request -- the model returned
// `finish_reason: tool_calls` and the exchange is over -- and the next round is a
// new request built from the history plus the tool result (see the loop in
// llm/openai_chat.cpp). Nothing is in flight while a tool runs, so the user may
// take as long as they like. What is held is one thread, ours.
//
// That is also the boundary of where this works: wherever the loop runs in-process
// (on-device, BYOK through the core, desktop). Behind an HTTP server the same wait
// would hold an SSE connection open across the user's think time, which proxies and
// mobile networks cut. There the tool is simply not registered and the model asks
// in prose -- no second protocol to maintain, just an absent tool.
//
// THREADING: expect() and wait() are called from the SAME thread -- the turn thread,
// first from the chat tier's AskFilter as it converts the call into an event the
// client can render, then from the tool handler itself. The pairing rides a
// thread-local, which is what lets the tool handler stay unaware of tool ids (its
// signature is (Args, UserInfo, ToolContext) and gains nothing by growing one).
// answer() and cancel_all() are called from other threads.

#include <string>

namespace mirobody { namespace mcp {

class AskBroker {
public:
    // The one broker. Process-wide because the answer arrives through the C ABI,
    // which has no turn handle to route by -- the ask id in the event is the route.
    static AskBroker& instance();

    // Chat tier, on the turn thread, before the tool runs: the next wait() on this
    // thread belongs to `ask_id`. Emitting the question is the caller's job.
    void expect(const std::string& ask_id);

    // Tool handler, on the same thread: block until the user answers, this turn is
    // cancelled, or `timeout_seconds` elapses.
    //
    // Returns the answer JSON the client sent. Returns EMPTY on timeout or
    // cancellation, which the caller must translate into something the model can act
    // on -- silence is not an answer, and a tool that hangs forever on a user who
    // walked away is worse than one that says nobody replied.
    std::string wait(int timeout_seconds);

    // Client side, any thread. False when nobody is waiting on that id: a stale
    // answer (the turn was cancelled, or the user tapped twice) is dropped, never
    // queued for whatever asks next.
    bool answer(const std::string& ask_id, const std::string& answer_json);

    // Abandon every outstanding wait -- a stopped turn, or shutdown. Waiters return
    // empty, exactly as on timeout.
    void cancel_all();

private:
    AskBroker() {}
    AskBroker(const AskBroker&);
    AskBroker& operator=(const AskBroker&);
};

}}
