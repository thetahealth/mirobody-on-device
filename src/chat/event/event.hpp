#pragma once

// Chat response events (chat interface tier).
//
// chat::Event is the chat service's *outbound* vocabulary: the unit a Responder
// sends back to the user (see chat/transport/responder.hpp). Each concrete event
// knows how to serialize itself to the wire JSON object, so a transport never
// switches on a type tag -- it just calls to_json() / to_sse().
//
// This is distinct from llm::Event, which is the *domain* event the LLM clients
// and agents produce (src/llm/event.hpp): llm is a lower layer that knows nothing
// about the chat service. The dispatcher translates the llm stream into chat
// events at the boundary via event_from_llm(); the chat tier also has events the
// domain doesn't (EndEvent today; session/ack events as the service grows).
//
// The wire JSON is {"type": <name>, "content": <text>} plus per-event extras:
// "tool_id" (tool steps), the nested "chart" / "cost" objects, "file" (an
// uploaded file's reference), and "phase"/"extracted" (text extraction). The
// shared fields stay byte-compatible with what the previous serializer emitted.

#include "llm/event.hpp"   // llm::Event, llm::CostStatistics (producer side)

#include <cstdint>
#include <memory>
#include <string>

namespace mirobody { namespace chat {

//------------------------------------------------------------------------------
// Abstract event
//------------------------------------------------------------------------------

class Event {
public:
    virtual ~Event() = default;

    // The wire type tag ("reply", "chart", "end", ...).
    virtual const char* type() const = 0;

    // The event as its wire JSON object string.
    virtual std::string to_json() const = 0;

    // The event as a Server-Sent Events message: `data: {json}\n\n`.
    std::string to_sse() const { return "data: " + to_json() + "\n\n"; }

    // The event as ONE string, for a transport that carries a flat
    // (type, content) pair instead of a JSON object -- the C ABI's
    // mirobody_chat_handler, which embedded clients drive directly.
    //
    // A text event returns its text. A structured event returns the part that
    // actually matters to a renderer (ChartEvent -> the ECharts option), NOT the
    // whole frame: an embedded client would otherwise have to unwrap a JSON
    // object to reach the object it wanted. The default is the full frame, which
    // is the honest answer for an event nobody has given a flat form yet.
    virtual std::string payload() const { return to_json(); }
};

//------------------------------------------------------------------------------
// Concrete events
//------------------------------------------------------------------------------

// Plain text payloads: a user-facing answer chunk and a reasoning chunk.
class ReplyEvent : public Event {
public:
    explicit ReplyEvent(std::string content) : content_(std::move(content)) {}
    const char* type() const override { return "reply"; }
    std::string to_json() const override;
    std::string payload() const override { return content_; }
private:
    std::string content_;
};

class ThinkingEvent : public Event {
public:
    explicit ThinkingEvent(std::string content) : content_(std::move(content)) {}
    const char* type() const override { return "thinking"; }
    std::string to_json() const override;
    std::string payload() const override { return content_; }
private:
    std::string content_;
};

// One step of a tool call: its title (name), its arguments, or its result. The
// three share a tool_id so the client can correlate them.
class ToolEvent : public Event {
public:
    enum class Phase { Title, Arguments, Detail };

    ToolEvent(Phase phase, std::string content, std::string tool_id)
        : phase_(phase), content_(std::move(content)), tool_id_(std::move(tool_id)) {}

    const char* type() const override;
    std::string to_json() const override;
    std::string payload() const override { return content_; }

    // Accessors so event filters can inspect a tool step (e.g. ChartFilter
    // recognizing a render_chart call across the Title/Arguments/Detail triple).
    Phase              phase()   const { return phase_; }
    const std::string& content() const { return content_; }
    const std::string& tool_id() const { return tool_id_; }
private:
    Phase       phase_;
    std::string content_;
    std::string tool_id_;
};

// An Apache ECharts option the frontend renders. `content` is an optional title;
// `option_json` is the raw ECharts option, emitted as a nested "chart" object.
class ChartEvent : public Event {
public:
    ChartEvent(std::string content, std::string option_json, std::string tool_id)
        : content_(std::move(content)), option_json_(std::move(option_json)),
          tool_id_(std::move(tool_id)) {}

    const char* type() const override { return "chart"; }
    std::string to_json() const override;
    // The ECharts option alone: what a renderer needs, ready for setOption().
    // The optional title is deliberately not carried -- no client reads it.
    std::string payload() const override { return option_json_; }
private:
    std::string content_;
    std::string option_json_;
    std::string tool_id_;
};

// A question the model put to the user through the ask_user tool, waiting for an
// answer. `content` is the question; `spec_json` carries the choices; `tool_id` is
// the id the client must echo back with the answer (mcp::AskBroker routes on it).
//
// Unlike every other event here this one expects a REPLY: the turn is parked inside
// the tool call until it arrives (see mcp/ask.hpp). A client that ignores this event
// leaves the turn waiting until the tool times out, which is why the timeout exists.
class AskEvent : public Event {
public:
    AskEvent(std::string content, std::string spec_json, std::string tool_id)
        : content_(std::move(content)), spec_json_(std::move(spec_json)),
          tool_id_(std::move(tool_id)) {}

    const char* type() const override { return "ask"; }
    std::string to_json() const override;
    // The whole question INCLUDING its id -- the C ABI's flat (type, content) pair
    // has nowhere else to put one, and an answer without the id cannot be routed.
    std::string payload() const override;
private:
    std::string content_;
    std::string spec_json_;
    std::string tool_id_;
};

// Terminal usage/cost summary for a turn.
class CostEvent : public Event {
public:
    explicit CostEvent(llm::CostStatistics cost) : cost_(std::move(cost)) {}
    const char* type() const override { return "costStatistics"; }
    std::string to_json() const override;
    // Empty, matching what the C ABI has always sent for this event.
    std::string payload() const override { return std::string(); }
private:
    llm::CostStatistics cost_;
};

// A server- or transport-level error message.
class ErrorEvent : public Event {
public:
    explicit ErrorEvent(std::string message) : message_(std::move(message)) {}
    const char* type() const override { return "error"; }
    std::string to_json() const override;
    std::string payload() const override { return message_; }
private:
    std::string message_;
};

// Upload-preprocess events. Chat-tier only -- the domain (llm) has no equivalent
// (so event_from_llm() never produces them); the dispatcher emits them directly
// from its attachment preprocess (Dispatcher::store_attachments), before the
// agent turn streams, so the client can show per-file progress.

// One uploaded attachment, offloaded to object storage. `content` is the
// (client) filename; the stored reference rides in a nested "file" object
// {filename, mime_type, url, file_key} -- the same shape params.files carries
// and the client already understands.
class UploadEvent : public Event {
public:
    UploadEvent(std::string filename, std::string mime_type,
                    std::string url, std::string file_key)
        : filename_(std::move(filename)), mime_type_(std::move(mime_type)),
          url_(std::move(url)), file_key_(std::move(file_key)) {}
    const char* type() const override { return "upload"; }
    std::string to_json() const override;
private:
    std::string filename_;
    std::string mime_type_;
    std::string url_;
    std::string file_key_;
};

// Text extraction from a non-text upload (image/PDF/...). Begin is emitted right
// before the (LLM-backed, possibly slow) extraction so the client can show a
// spinner; Done after it, with `extracted` telling whether any text resulted.
// `content` is the filename. The "phase" tag distinguishes the two; "extracted"
// is present on Done only.
class TranscriptEvent : public Event {
public:
    enum class Phase { Begin, Done };

    TranscriptEvent(Phase phase, std::string filename, bool extracted = false)
        : phase_(phase), filename_(std::move(filename)), extracted_(extracted) {}

    const char* type() const override { return "transcript"; }
    std::string to_json() const override;
private:
    Phase       phase_;
    std::string filename_;
    bool        extracted_;
};

// The durable conversation (thread) id for this turn. Chat-tier only -- the
// dispatcher emits it once, right after persisting the question, so the client
// can remember it and (a) echo it back as conversation_id to continue the same
// thread, and (b) share it with a care-circle member. `content` carries the id
// as a decimal string (precise for JS, which loses integer precision past 2^53);
// "conversation_id" repeats it as a number for convenience.
class ConversationEvent : public Event {
public:
    explicit ConversationEvent(std::int64_t id) : id_(id) {}
    const char* type() const override { return "conversation"; }
    std::string to_json() const override;
private:
    std::int64_t id_;
};

// The end-of-turn marker the frontend waits for. Chat-tier only -- the domain
// (llm) has no such event; a Responder emits it from finish().
class EndEvent : public Event {
public:
    const char* type() const override { return "end"; }
    std::string to_json() const override;
};

//------------------------------------------------------------------------------
// Adapter
//------------------------------------------------------------------------------

// Translate one domain (llm) event into its chat response event. Returns null
// for an llm event type the chat tier does not surface. The dispatcher calls
// this at the transport boundary so Responders only ever see chat::Event.
std::unique_ptr<Event> event_from_llm(const llm::Event& e);

}}
