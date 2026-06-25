#pragma once

#include "chat/chat.hpp"
#include "chat/dispatcher.hpp"
#include "chat/transport/transport.hpp"
#include "cache/cache.hpp"
#include "config/config.hpp"
#include "database/database.hpp"
#include "transcode/parser.hpp"
#include "storage/storage.hpp"
#include "jwt/jwt.hpp"
#include "server/router.hpp"

#include <memory>
#include <vector>

namespace mirobody {
namespace memory { class Memory; }
namespace chat {

// Chat service composition root.
//
// ChatService wires the chat tiers together: it owns the domain (chat::Chat), the
// chat::Dispatcher that routes parsed requests to it, and one chat::Transport per
// enabled wire protocol (SSE, WebSocket, ...; see chat/transport/). At
// construction it starts every transport, then registers the plain REST endpoints
// that are not streaming transports (provider discovery and the history log).
// Callers instantiate it once at startup:
//
//     chat::ChatService chat_svc(router, cfg, db, cache, storage, jwt);
//
// The service *borrows* `cfg`, `db`, `cache`, `storage`, and `jwt` (it does not
// own them): `cfg`/`db` build the Chat, `storage` is handed to the SSE transport
// for uploads (may be null), and `jwt` guards the transports' routes and the REST
// endpoints. Lifetime contract: the borrowed objects and the Router (and
// therefore this service and its transports, since their handlers capture it)
// must outlive the running server.
class ChatService {
public:
    ChatService(server::Router& router,
                const Config& cfg,
                database::Database& db,
                cache::Cache& cache,
                storage::Storage* storage,
                memory::Memory* memory,
                const jwt::Jwt& jwt);

    ChatService(const ChatService&)            = delete;
    ChatService& operator=(const ChatService&) = delete;

private:
    // Registers the non-streaming REST routes (/api/providers, /api/history,
    // /api/history/delete). The streaming transports are owned by transports_.
    void register_rest_routes(server::Router& router);

    // GET|POST /api/providers -- "agent/provider" pairs that have a client loaded.
    void handle_providers(const server::Request& req, server::Response& res);
    // GET|POST /api/history -- the caller's past sessions (newest first),
    // paginated by ?page / ?page_size.
    void handle_history(const server::Request& req, server::Response& res);
    // POST /api/history/delete -- remove one of the caller's sessions by id.
    void handle_history_delete(const server::Request& req, server::Response& res);
    // GET|POST /api/files -- the caller's uploaded files (newest first), each
    // with a signed `url` (raw bytes) and `text_url` (extracted text, when one
    // exists), both fetchable directly by the browser. Empty when no object
    // store is configured.
    void handle_files(const server::Request& req, server::Response& res);

    // Remove one of the caller's sessions by id. Returns false for a bad id /
    // user; throws std::exception on a database error.
    bool delete_history(std::int64_t user_id, const std::string& session_id);

    Chat                                     chat_;        // domain core (Tier 2)
    std::unique_ptr<file::Parser>            parser_;      // upload text extraction
    Dispatcher                               dispatcher_;  // routes Packet -> chat_
    database::Database&                      db_;          // history + files-table queries
    storage::Storage*                        storage_;     // borrowed; null when no object store (mints file URLs)
    const jwt::Jwt&                          jwt_;         // guards the REST routes
    std::vector<std::unique_ptr<Transport> > transports_;  // one per enabled protocol
};

}}
