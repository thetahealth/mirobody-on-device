#pragma once

// Care-circle service (chat interface tier sibling).
//
// Owns the care-circle social graph and conversation sharing HTTP routes:
//   - /api/circle/create | rename | delete
//   - /api/circle/invite | members | accept | decline | remove
//   - /api/conversation/share | unshare | shares
//
// A care circle is a group (res/sql/*/2_care_circle.sql): a user owns any number
// of them, invites others by email into a chosen circle (acceptance required), and
// every accepted member is mutually in that circle. Members can then grant each
// other read/edit access to individual conversations (conversation_shares; the
// read side lives in chat::ChatService). Sharing/health-read authorization joins
// across all of a user's circles, so being co-members of any one circle suffices.
//
// Modern backends only -- the schema exists only there -- so the server does NOT
// construct this on the legacy backend. Borrows cfg/db/jwt; every route is
// JWT-guarded. Lifetime: the borrowed objects and the Router must outlive the
// running server.

#include "cache/cache.hpp"
#include "config/config.hpp"
#include "database/database.hpp"
#include "jwt/jwt.hpp"
#include "server/router.hpp"

#include <cstdint>
#include <string>

namespace mirobody {
namespace server { class Request; class Response; }
namespace circle {

class CircleService {
public:
    CircleService(server::Router& router,
                  const Config& cfg,
                  database::Database& db,
                  cache::Cache& cache,
                  const jwt::Jwt& jwt);

    CircleService(const CircleService&)            = delete;
    CircleService& operator=(const CircleService&) = delete;

private:
    void register_routes(server::Router& router);

    // Care-circle graph.
    void handle_create(const server::Request& req, server::Response& res);
    void handle_rename(const server::Request& req, server::Response& res);
    void handle_delete(const server::Request& req, server::Response& res);
    void handle_invite(const server::Request& req, server::Response& res);
    void handle_members(const server::Request& req, server::Response& res);
    void handle_accept(const server::Request& req, server::Response& res);
    void handle_decline(const server::Request& req, server::Response& res);
    void handle_remove(const server::Request& req, server::Response& res);
    // Set/clear the display nickname for one member of a circle the caller
    // administers (care_circle_members.nickname). {care_circle_id, member_user_id, nickname}.
    void handle_set_nickname(const server::Request& req, server::Response& res);
    // Owner-only: set a member's role to Member or Maintainer (never Owner, never
    // the owner's own row). {care_circle_id, member_user_id, role}.
    void handle_set_role(const server::Request& req, server::Response& res);
    // Set the caller's own health-data sharing level (care_circle_members
    // .health_access: off / View / Edit) for one circle. care_circle_id is
    // required and the caller must be an accepted member of it -- sharing is
    // per circle, so there is no "all my circles at once" path.
    void handle_health_sharing(const server::Request& req, server::Response& res);
    // List the people who have shared their health data with the caller (the
    // inverse of health_sharing), for the chat "currently for" picker.
    void handle_health_shared_with_me(const server::Request& req, server::Response& res);
    // Conversation sharing.
    void handle_share(const server::Request& req, server::Response& res);
    void handle_unshare(const server::Request& req, server::Response& res);
    void handle_shares(const server::Request& req, server::Response& res);

    // Create a new circle owned by owner_id (named `name`) with the owner's own
    // Accepted membership, in one transaction. Returns the new id, or 0 on failure.
    std::int64_t create_circle(std::int64_t owner_id, const std::string& name);
    // The caller's default owned circle id (lowest id), creating "My circle" on
    // first use. The fallback target when invite/remove name no specific circle.
    std::int64_t ensure_owned_circle(std::int64_t owner_id);
    // The caller's default owned circle id (lowest id) without creating it, or 0.
    std::int64_t owned_circle(std::int64_t owner_id);
    // How many active circles owner_id owns (for the CIRCLE_MAX_PER_USER cap).
    int owned_circle_count(std::int64_t owner_id);
    // How many active members (any status, pending included) circle_id has (for
    // the CIRCLE_MAX_MEMBERS cap).
    int circle_member_count(std::int64_t circle_id);
    // Does owner_id own active circle circle_id? Guards owner-only mutations
    // (rename / delete / role change).
    bool owns_circle(std::int64_t owner_id, std::int64_t circle_id);
    // The caller's CircleRole in circle_id if an active, Accepted member, else -1.
    int  member_role(std::int64_t user_id, std::int64_t circle_id);
    // Is the caller an admin (Owner or Maintainer) of circle_id? Gates invite /
    // remove. (Maintainers are limited to removing plain Members; see handle_remove.)
    bool can_admin_circle(std::int64_t user_id, std::int64_t circle_id);
    // Soft-delete every active conversation share between a and b, both directions.
    void revoke_shares_between(std::int64_t a, std::int64_t b, std::int64_t now);
    // Resolve an email to a user id, creating a shell users row if absent
    // (mirrors user::add_or_get_user's modern path + lowercase/trim). 0 on failure.
    std::int64_t resolve_or_create_user(const std::string& email);
    // Are `a` and `b` both Accepted members of some shared circle?
    bool in_circle_together(std::int64_t a, std::int64_t b);
    // Resolve an opaque member handle (care_circle_members.id) to its circle and
    // user. Returns false when the handle is unknown / soft-deleted. Lets the
    // mutation routes take a handle from the client instead of a raw users PK.
    bool resolve_member(std::int64_t member_id, std::int64_t* circle_id, std::int64_t* user_id);
    // Best-effort: email `to_email` an invite carrying the accept link/token.
    // Logged and swallowed on any failure (no transport, send error) -- the
    // invite row already exists and can be accepted in-app regardless.
    void send_invite_email(const std::string& to_email, const std::string& inviter_email,
                           const std::string& token);
    // Does `owner_id` own conversation `conversation_id` (active)?
    bool owns_conversation(std::int64_t owner_id, const std::string& conversation_id);

    const Config&       cfg_;
    database::Database& db_;
    cache::Cache&       cache_;
    const jwt::Jwt&     jwt_;
};

}}
