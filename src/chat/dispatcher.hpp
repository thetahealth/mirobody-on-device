#pragma once

// Dispatcher -- routes a parsed request to the chat domain (chat interface tier).
//
// This is the transport-neutral seam between the transports and chat::Chat. A
// transport hands it a chat::Packet (the parsed request, plus any binary
// attachments) and a chat::Responder (the request's output channel). The
// dispatcher first runs a preprocess that offloads attachment bytes to object
// storage and records the references in params.files; it then switches on
// Packet::code() to the matching command, parses the typed params (ChatParams /
// LiveParams), calls the Chat method, and streams the resulting events into the
// Responder -- finishing the turn with Responder::finish() exactly once. Bad
// requests (unknown code, missing agent/question) are reported as Error events
// through the Responder, not thrown.
//
// One instance serves every transport and every concurrent turn. It borrows the
// Chat and the object store (both must outlive it; storage may be null).

#include "chat/chat.hpp"    // Chat

#include <cstdint>
#include <string>

namespace mirobody {
namespace storage { class Storage; }
namespace cache { class Cache; }
namespace database { class Database; }
namespace memory { class Memory; }
namespace file { class Parser; }
namespace chat {

class Packet;
class Responder;

// Operation codes carried by a Packet and routed here. Streaming operations
// only -- the unary REST reads (providers / history) are handled directly by
// ChatService, not through a transport/dispatcher.
enum Op {
    kOpUnknown = -1,
    kOpChat    = 1,   // streaming agent turn   (SSE; Chat::response)
};

class Dispatcher {
public:
    // `chat` is borrowed; `storage` (the object store for uploaded attachments)
    // is borrowed and may be null (attachments then carry metadata only).
    // `cache` (the per-user recent-uploads index, see transcode/) is borrowed and
    // may be null (uploads then aren't indexed for MCP resources/list).
    // `parser` extracts text from non-text uploads at store time (see
    // transcode/parser.hpp); borrowed and may be null (uploads then carry no text).
    // `db` is borrowed and may be null (circle subject resolution and the
    // uploads index then degrade).
    // `memory` (the long-term memory store, see memory/) is borrowed and may be
    // null (the `remember` / `recall_memory` tools then report memory is off);
    // it is threaded onto each turn's AgentRequest so the tool executor reaches it.
    // `rate_max` / `rate_window_seconds` cap agent turns per user per window
    // (rate_max <= 0 disables the limit); enforced via `cache` (no cache => no
    // limit). See CHAT_RATE_MAX / CHAT_RATE_WINDOW_SEC.
    Dispatcher(Chat& chat, storage::Storage* storage, cache::Cache* cache,
               file::Parser* parser, database::Database* db, memory::Memory* memory,
               int rate_max = 0, int rate_window_seconds = 60);

    Dispatcher(const Dispatcher&)            = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;

    // Run the request in `pkt` for the identified caller, streaming its events
    // into `out`. `user_id` is the JWT-verified caller (0 when the transport
    // carries no identity, e.g. the live socket); it is passed separately so a
    // request body can never forge it. The rest of the turn context (session_id,
    // timezone, ...) the transport stamps into `pkt`. `pkt` is mutated by the
    // upload preprocess. Always calls out.finish() before returning.
    void dispatch(Packet& pkt, std::int64_t user_id, Responder& out);

private:
    // Preprocess: store each of pkt's binary attachments in object storage and
    // append a {filename, mime_type, url, file_key} reference to params.files,
    // so the command's params struct reads files uniformly regardless of how the
    // bytes arrived (HTTP multipart, WS binary frame, ...). Emits a Upload
    // event per stored file (and a Transcript begin/done pair around each
    // text extraction) into `out`, so the client sees per-file progress before
    // the turn streams.
    void store_attachments(Packet& pkt, std::int64_t user_id, Responder& out) const;

    // True if `user_id` may run another turn now (under the per-user rate limit);
    // false once the window cap is hit. Always true when the limit is disabled,
    // there's no cache, or user_id <= 0 (an unidentifiable caller can't be keyed).
    bool allow_turn(std::int64_t user_id) const;

    Chat&               chat_;
    storage::Storage*   storage_;
    cache::Cache*       cache_;
    file::Parser*       parser_;
    database::Database* db_;
    memory::Memory*     memory_;
    int                 rate_max_;
    int                 rate_window_seconds_;
};

}}
