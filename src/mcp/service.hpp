#pragma once

#include "cache/cache.hpp"
#include "config/config.hpp"
#include "jwt/jwt.hpp"
#include "mcp/tool.hpp"
#include "server/router.hpp"

namespace mirobody {
namespace storage  { class Storage; }
namespace memory   { class Memory; }
namespace database { class Database; }
namespace mcp {

// MCP JSON-RPC endpoint. The C++ port of the dispatch half of the Python
// reference's mcp/service.py: it owns the POST /mcp route, parses the JSON-RPC
// 2.0 envelope, and routes the standard MCP methods (initialize, tools/list,
// tools/call, ping, ...) to the compile-time tool registry (see mcp/tool.hpp).
//
// Constructing the service wires its route onto the Router, mirroring the
// ChatService pattern:
//
//     mcp::McpService mcp_svc(router, cfg, jwt);
//
// `cfg`, `jwt`, and `cache` are *borrowed* (not owned) and must outlive the
// running server. `jwt` and `cache` are used to resolve the caller's identity
// for tools that declare `auth = true`; unauthenticated discovery (initialize
// / tools/list) needs neither. A caller authenticates in one of two ways:
//
//   - Bearer JWT on the POST /mcp request (the Authorization header), or
//   - a personal-MCP secret embedded in the URL (POST /mcp/{secret}). The
//     secret is minted by POST /personal/mcp and mapped to a user id in the
//     cache; the secret URL lets an MCP client that cannot send a bearer token
//     authenticate by configuring a single long-lived URL.
//
// Scope notes vs. the Python reference, still deferred:
//   - Per-agent tool filtering (ALLOWED_TOOLS_* / DISALLOWED_TOOLS_*).
//   - The OAuth login-URL + polling response when an auth tool is called with
//     no identity (we return a plain "Authentication required" error).
//   - Beneficiary / relationship delegation in /personal/mcp (no
//     check_relationship port yet), and the temporary-secret variant.
//
// resources/list and resources/read serve two kinds of resource: the static,
// compile-time ones from the resource registry (see mcp/resource.hpp), and the
// caller's own uploaded files. The latter are indexed per user in the cache by
// the chat upload path (see transcode/) and read back as `file://<key>` URIs,
// whose bytes are fetched from object storage on demand (hence `storage`).
class McpService {
public:
    McpService(server::Router& router, const Config& cfg,
               const jwt::Jwt& jwt, cache::Cache& cache,
               storage::Storage* storage, memory::Memory* memory,
               database::Database* db);

    McpService(const McpService&)            = delete;
    McpService& operator=(const McpService&) = delete;

private:
    void register_routes(server::Router& router);

    // The POST /mcp[/{secret}] handler: parse, dispatch, respond.
    void handle(const server::Request& req, server::Response& res);

    // POST /personal/mcp: mint (or reuse) the caller's personal MCP URL.
    void generate_personal_mcp(const server::Request& req, server::Response& res);

    // Resolve a personal-MCP secret to its user id via the cache, or "" when
    // the secret is unknown / expired.
    std::string user_for_secret(const std::string& secret);

    // Auth gate for an auth-flagged method (tools/call, resources/read) -- the
    // caller guards the call on the entry's `auth` flag. Identity was already
    // resolved into req.user_id by handle() (path secret or bearer token); this
    // mirrors it into `user` and returns true when present. When absent it
    // writes a 401 + WWW-Authenticate RFC 9728 challenge (plus a JSON-RPC error
    // body) and returns false so the caller stops.
    bool authenticate(const server::Request& req, server::Response& res,
                      const std::string& id, UserInfo& user);

    const Config&     cfg_;
    const jwt::Jwt&   jwt_;
    cache::Cache&     cache_;
    storage::Storage*   storage_;   // borrowed; null when no object store configured
    memory::Memory*     memory_;    // borrowed; null when memory is disabled
    database::Database* db_;        // borrowed relational store (chat-history tools)

    std::string protocol_version_;
    std::string name_;
    std::string version_;
};

}}
