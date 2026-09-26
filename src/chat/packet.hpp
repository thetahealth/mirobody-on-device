#pragma once

// Packet -- a generic inbound-request envelope (chat interface tier).
//
// A transport adapter's job on the way in is to turn whatever its wire delivers
// (an HTTP body, a WebSocket frame, an MQTT message) into a Packet: an integer
// `code` naming the operation plus a JSON `params` object carrying its arguments.
// The dispatcher (chat::Dispatcher) then switches on code() and reads the args,
// so nothing past the transport boundary cares how the bytes arrived.
//
// A Packet OWNS its params object (a rapidjson::Document): the body / frame it
// was built from is transient, so the envelope keeps its own copy. It is movable
// but not copyable, like the Document it wraps.
//
// Two ways to build one:
//   - Packet::parse(text)         for a {"code", "params"} wire payload.
//   - Packet(code) + the builder  for a transport that normalizes its own native
//     body (e.g. an HTML form) into params field by field, or
//     Packet(code, document)      to adopt an already-parsed object as params.
//
// The typed getters mirror mcp::Args and read directly from the params object;
// they are tolerant (a missing / wrong-typed key yields the supplied default).


#include <rapidjson/document.h>

#include <string>
#include <vector>
#include "blob.hpp"

namespace mirobody { namespace chat {

// A binary attachment carried alongside a Packet -- raw uploaded bytes (a
// multipart file part, a WebSocket binary frame, ...). Bytes are deliberately
// NOT put into the JSON params: the dispatcher's preprocess offloads each
// attachment to object storage and rewrites params.files with a reference before
// the command runs, so params stays small and the bytes never pass through JSON.
struct Attachment {
    std::string filename;    // client-supplied name; becomes the stored object's last segment
    std::string mime_type;   // declared content type ("" -> application/octet-stream when stored)

    // The raw bytes, shared rather than owned outright (see mirobody::Blob).
    // They are read by the dispatcher (to store and to extract text from) and by
    // the params parse that hands them to the agent, all through const
    // references -- so a by-value std::string here meant a full copy at each of
    // those hops. Read them with data()/size()/str(), never c_str(): this is a
    // byte buffer and embedded NULs are expected. The buffer underneath is a
    // std::string because every producer and consumer on this path already
    // speaks one (the multipart part in, storage::put_object out), so it moves
    // in and out with no conversion.
    Blob data;
};

class Packet {
public:
    // Sentinel code for an absent / unparseable / unset operation.
    static const int kNoCode = -1;

    // Empty packet: kNoCode, empty params object.
    Packet();
    // Packet with `code` and an empty params object, to be filled via the
    // builder API (params_mut() / allocator()).
    explicit Packet(int code);
    // Packet with `code` adopting `params_object` as its params (moved in). A
    // non-object value is replaced with an empty object.
    Packet(int code, rapidjson::Document params_object);

    // Parse a {"code", "params"} JSON object from `text`. Malformed / non-object
    // input yields an empty packet (code() == kNoCode, empty params).
    static Packet parse(const std::string& text);

    Packet(Packet&&)                 = default;
    Packet& operator=(Packet&&)      = default;
    Packet(const Packet&)            = delete;
    Packet& operator=(const Packet&) = delete;

    int  code() const { return code_; }
    void set_code(int c) { code_ = c; }
    bool ok()   const { return code_ != kNoCode; }

    //--------------------------------------------------------------------------
    // Typed read-only access to a params key, applying `def` when the key is
    // missing or holds the wrong type. Tolerant everywhere.
    //--------------------------------------------------------------------------
    bool                     has(const char* key) const;
    std::string              str(const char* key, const std::string& def = std::string()) const;
    long long                integer(const char* key, long long def = 0) const;
    double                   number(const char* key, double def = 0.0) const;
    bool                     boolean(const char* key, bool def = false) const;
    std::vector<std::string> string_array(const char* key) const;

    // The params object itself, for arguments whose shape the typed getters do
    // not cover (e.g. a nested "messages" array). Always an object.
    const rapidjson::Value& params() const { return doc_; }

    //--------------------------------------------------------------------------
    // Builder API -- for a transport normalizing a native body into params.
    //--------------------------------------------------------------------------
    rapidjson::Value&                    params_mut() { return doc_; }
    rapidjson::Document::AllocatorType&  allocator()  { return doc_.GetAllocator(); }

    //--------------------------------------------------------------------------
    // Binary attachments -- raw bytes a transport carries in (not in params).
    // The dispatcher preprocess consumes these into object storage.
    //--------------------------------------------------------------------------
    const std::vector<Attachment>& attachments() const { return attachments_; }
    void add_attachment(Attachment a) { attachments_.push_back(std::move(a)); }

private:
    int                     code_;
    rapidjson::Document     doc_;          // the params object (always an object)
    std::vector<Attachment> attachments_;  // raw bytes; offloaded by the dispatcher
};

}}
