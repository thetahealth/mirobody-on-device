#include "circle/service.hpp"
#include "circle/access.hpp"

#include "database/enums.hpp"   // CircleRole, CircleStatus, ShareAccess
#include "platform/clock.hpp"   // now_unix_ms
#include "platform/log.hpp"
#include "server/auth.hpp"
#include "server/request.hpp"
#include "server/response.hpp"
#include "user/email.hpp"        // EmailValidatorOptions, send_email

#include <openssl/rand.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cctype>
#include <chrono>
#include <cstddef>
#include <random>
#include <string>
#include <vector>

namespace mirobody { namespace circle {

namespace db = database;

namespace {

const int kOwner      = static_cast<int>(db::CircleRole::Owner);
const int kMaintainer = static_cast<int>(db::CircleRole::Maintainer);
const int kMember     = static_cast<int>(db::CircleRole::Member);
const int kPending  = static_cast<int>(db::CircleStatus::Pending);
const int kAccepted = static_cast<int>(db::CircleStatus::Accepted);
const int kDeclined = static_cast<int>(db::CircleStatus::Declined);
const int kView     = static_cast<int>(db::ShareAccess::View);
const int kEdit     = static_cast<int>(db::ShareAccess::Edit);

// Read a string member from a JSON object body ("" if absent / wrong type). A
// number is returned as decimal text so id binding stays uniform.
std::string body_str(const std::string& body, const char* key) {
    rapidjson::Document d;
    if (d.Parse(body.c_str()).HasParseError() || !d.IsObject()) return std::string();
    rapidjson::Value::ConstMemberIterator it = d.FindMember(key);
    if (it == d.MemberEnd()) return std::string();
    if (it->value.IsString()) return std::string(it->value.GetString(), it->value.GetStringLength());
    if (it->value.IsInt64() || it->value.IsInt()) return std::to_string(it->value.GetInt64());
    return std::string();
}

// Opaque member handles (care_circle_members.id) from the body: a `members`
// number/string array and/or a singular `member`. Non-positive entries dropped.
// The mutation routes take these instead of a raw users PK.
std::vector<std::int64_t> body_handles(const std::string& body) {
    std::vector<std::int64_t> out;
    rapidjson::Document d;
    if (d.Parse(body.c_str()).HasParseError() || !d.IsObject()) return out;
    rapidjson::Value::ConstMemberIterator it = d.FindMember("members");
    if (it != d.MemberEnd() && it->value.IsArray()) {
        for (rapidjson::SizeType i = 0; i < it->value.Size(); ++i) {
            const rapidjson::Value& v = it->value[i];
            std::int64_t id = 0;
            if (v.IsInt64() || v.IsInt())   id = v.GetInt64();
            else if (v.IsString())          id = std::strtoll(v.GetString(), nullptr, 10);
            if (id > 0) out.push_back(id);
        }
    }
    rapidjson::Value::ConstMemberIterator s = d.FindMember("member");
    if (s != d.MemberEnd()) {
        std::int64_t id = 0;
        if (s->value.IsInt64() || s->value.IsInt()) id = s->value.GetInt64();
        else if (s->value.IsString())               id = std::strtoll(s->value.GetString(), nullptr, 10);
        if (id > 0) out.push_back(id);
    }
    return out;
}

// Trim surrounding whitespace (no case change) -- for circle names.
std::string trim_ws(std::string s) {
    const std::size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    const std::size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Trim surrounding whitespace and lowercase -- the same normalization the email
// login path applies, so an invited address matches the user the invitee later
// signs in as.
std::string norm_email(std::string s) {
    const std::size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    const std::size_t e = s.find_last_not_of(" \t\r\n");
    s = s.substr(b, e - b + 1);
    for (std::size_t i = 0; i < s.size(); ++i)
        s[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    return s;
}

// 128 bits of randomness as lowercase hex (32 chars), the invite link token.
std::string random_token() {
    unsigned char b[16];
    if (RAND_bytes(b, sizeof(b)) != 1) {
        std::random_device rd;
        for (std::size_t i = 0; i < sizeof(b); ++i) b[i] = static_cast<unsigned char>(rd());
    }
    static const char kHex[] = "0123456789abcdef";
    std::string s;
    s.reserve(sizeof(b) * 2);
    for (std::size_t i = 0; i < sizeof(b); ++i) {
        s.push_back(kHex[b[i] >> 4]);
        s.push_back(kHex[b[i] & 0x0F]);
    }
    return s;
}

// Does `target` grant `viewer` health access of at least `min_level`
// (ShareAccess) through some shared circle? Both must be Accepted, active
// members of the same circle and target.health_access >= min_level. Caller
// handles the self / legacy cases. Never throws (false on error).
bool circle_health_grants(database::Database& conn, std::int64_t viewer,
                          std::int64_t target, int min_level) {
    try {
        database::Result r = conn.execute(
            "SELECT 1 FROM care_circle_members me "
            "JOIN care_circle_members owner ON me.care_circle_id=owner.care_circle_id "
            "WHERE me.user_id=? AND owner.user_id=? AND me.status=? AND owner.status=? "
            "AND owner.health_access >= ? AND me.deleted_at IS NULL AND owner.deleted_at IS NULL LIMIT 1;",
            {std::to_string(viewer), std::to_string(target), kAccepted, kAccepted, min_level});
        return !r.rows.empty() && !r.rows[0].empty();
    } catch (const std::exception&) {
        return false;
    }
}

// CircleStatus int -> wire string.
const char* status_str(int s) {
    if (s == kAccepted) return "accepted";
    if (s == kDeclined) return "declined";
    return "pending";
}

void ok_json(server::Response& res, rapidjson::Document& d) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    d.Accept(w);
    res.ok(std::string(buf.GetString(), buf.GetSize()));
}

}   // namespace

//------------------------------------------------------------------------------

CircleService::CircleService(server::Router& router,
                             const Config& cfg,
                             database::Database& db,
                             cache::Cache& cache,
                             const jwt::Jwt& jwt)
    : cfg_(cfg), db_(db), cache_(cache), jwt_(jwt) {
    register_routes(router);
}

void CircleService::register_routes(server::Router& router) {
    router.post("/api/circle/create", server::require_auth(jwt_,
        [this](const server::Request& q, server::Response& s) { handle_create(q, s); }));
    router.post("/api/circle/rename", server::require_auth(jwt_,
        [this](const server::Request& q, server::Response& s) { handle_rename(q, s); }));
    router.post("/api/circle/delete", server::require_auth(jwt_,
        [this](const server::Request& q, server::Response& s) { handle_delete(q, s); }));
    router.post("/api/circle/invite", server::require_auth(jwt_,
        [this](const server::Request& q, server::Response& s) { handle_invite(q, s); }));
    router.http("/api/circle/members", server::require_auth(jwt_,
        [this](const server::Request& q, server::Response& s) { handle_members(q, s); }),
        server::GET | server::POST);
    router.post("/api/circle/accept", server::require_auth(jwt_,
        [this](const server::Request& q, server::Response& s) { handle_accept(q, s); }));
    router.post("/api/circle/decline", server::require_auth(jwt_,
        [this](const server::Request& q, server::Response& s) { handle_decline(q, s); }));
    router.post("/api/circle/remove", server::require_auth(jwt_,
        [this](const server::Request& q, server::Response& s) { handle_remove(q, s); }));
    router.post("/api/circle/nickname", server::require_auth(jwt_,
        [this](const server::Request& q, server::Response& s) { handle_set_nickname(q, s); }));
    router.post("/api/circle/role", server::require_auth(jwt_,
        [this](const server::Request& q, server::Response& s) { handle_set_role(q, s); }));
    router.post("/api/circle/health-sharing", server::require_auth(jwt_,
        [this](const server::Request& q, server::Response& s) { handle_health_sharing(q, s); }));
    router.http("/api/circle/health-shared-with-me", server::require_auth(jwt_,
        [this](const server::Request& q, server::Response& s) { handle_health_shared_with_me(q, s); }),
        server::GET | server::POST);

    router.post("/api/conversation/share", server::require_auth(jwt_,
        [this](const server::Request& q, server::Response& s) { handle_share(q, s); }));
    router.post("/api/conversation/unshare", server::require_auth(jwt_,
        [this](const server::Request& q, server::Response& s) { handle_unshare(q, s); }));
    router.http("/api/conversation/shares", server::require_auth(jwt_,
        [this](const server::Request& q, server::Response& s) { handle_shares(q, s); }),
        server::GET | server::POST);
}

//------------------------------------------------------------------------------
// Helpers
//------------------------------------------------------------------------------

std::int64_t CircleService::resolve_or_create_user(const std::string& email) {
    const std::string lower = norm_email(email);
    if (lower.empty() || lower.find('@') == std::string::npos) return 0;
    try {
        db::Result sel = db_.execute(
            "SELECT id FROM users WHERE email=? AND deleted_at IS NULL;", {lower});
        if (!sel.rows.empty() && !sel.rows[0].empty() && !sel.rows[0][0].is_null())
            return sel.rows[0][0].as_int();

        const std::string name = lower.substr(0, lower.find('@'));
        const std::int64_t now = platform::now_unix_ms();
#if defined(MIROBODY_DATABASE_PG)
        db::Result ins = db_.execute(
            "INSERT INTO users (email, name, created_at) VALUES (?, ?, ?) RETURNING id;",
            {lower, name, now});
        return (ins.rows.empty() || ins.rows[0].empty()) ? 0 : ins.rows[0][0].as_int();
#else
        db::Result ins = db_.execute(
            "INSERT INTO users (email, name, created_at) VALUES (?, ?, ?);",
            {lower, name, now});
        return ins.last_insert_id;
#endif
    } catch (const std::exception& e) {
        platform::log_warn("circle: resolve_or_create_user failed: %s", e.what());
        return 0;
    }
}

std::int64_t CircleService::owned_circle(std::int64_t owner_id) {
    db::Result r = db_.execute(
        "SELECT id FROM care_circles WHERE owner_user_id=? AND deleted_at IS NULL "
        "ORDER BY id LIMIT 1;",
        {std::to_string(owner_id)});
    if (r.rows.empty() || r.rows[0].empty() || r.rows[0][0].is_null()) return 0;
    return r.rows[0][0].as_int();
}

int CircleService::owned_circle_count(std::int64_t owner_id) {
    db::Result r = db_.execute(
        "SELECT COUNT(*) FROM care_circles WHERE owner_user_id=? AND deleted_at IS NULL;",
        {std::to_string(owner_id)});
    if (r.rows.empty() || r.rows[0].empty() || r.rows[0][0].is_null()) return 0;
    return static_cast<int>(r.rows[0][0].as_int());
}

int CircleService::circle_member_count(std::int64_t circle_id) {
    db::Result r = db_.execute(
        "SELECT COUNT(*) FROM care_circle_members WHERE care_circle_id=? AND deleted_at IS NULL;",
        {std::to_string(circle_id)});
    if (r.rows.empty() || r.rows[0].empty() || r.rows[0][0].is_null()) return 0;
    return static_cast<int>(r.rows[0][0].as_int());
}

std::int64_t CircleService::create_circle(std::int64_t owner_id, const std::string& name) {
    const std::string  oid = std::to_string(owner_id);
    const std::int64_t now = platform::now_unix_ms();
    db::Transaction tx = db_.begin();
#if defined(MIROBODY_DATABASE_PG)
    db::Result r = tx.execute(
        "INSERT INTO care_circles (owner_user_id, name, created_at) VALUES (?, ?, ?) RETURNING id;",
        {oid, name, now});
    const std::int64_t cid = (r.rows.empty() || r.rows[0].empty()) ? 0 : r.rows[0][0].as_int();
#else
    db::Result r = tx.execute(
        "INSERT INTO care_circles (owner_user_id, name, created_at) VALUES (?, ?, ?);",
        {oid, name, now});
    const std::int64_t cid = r.last_insert_id;
#endif
    if (cid <= 0) return 0;
    // The owner is a member of their own circle: role Owner, already Accepted.
    tx.execute(
        "INSERT INTO care_circle_members (care_circle_id, user_id, role, status, created_at) "
        "VALUES (?, ?, ?, ?, ?);",
        {cid, oid, kOwner, kAccepted, now});
    tx.commit();
    return cid;
}

std::int64_t CircleService::ensure_owned_circle(std::int64_t owner_id) {
    const std::int64_t existing = owned_circle(owner_id);
    if (existing > 0) return existing;
    return create_circle(owner_id, "My circle");
}

bool CircleService::owns_circle(std::int64_t owner_id, std::int64_t circle_id) {
    if (owner_id <= 0 || circle_id <= 0) return false;
    db::Result r = db_.execute(
        "SELECT 1 FROM care_circles WHERE id=? AND owner_user_id=? AND deleted_at IS NULL;",
        {std::to_string(circle_id), std::to_string(owner_id)});
    return !r.rows.empty() && !r.rows[0].empty();
}

int CircleService::member_role(std::int64_t user_id, std::int64_t circle_id) {
    if (user_id <= 0 || circle_id <= 0) return -1;
    db::Result r = db_.execute(
        "SELECT role FROM care_circle_members "
        "WHERE care_circle_id=? AND user_id=? AND status=? AND deleted_at IS NULL;",
        {std::to_string(circle_id), std::to_string(user_id), kAccepted});
    if (r.rows.empty() || r.rows[0].empty() || r.rows[0][0].is_null()) return -1;
    return static_cast<int>(r.rows[0][0].as_int());
}

bool CircleService::can_admin_circle(std::int64_t user_id, std::int64_t circle_id) {
    return member_role(user_id, circle_id) >= kMaintainer;   // Maintainer or Owner
}

void CircleService::revoke_shares_between(std::int64_t a, std::int64_t b, std::int64_t now) {
    const std::string as = std::to_string(a), bs = std::to_string(b);
    db_.execute(
        "UPDATE conversation_shares SET deleted_at=? "
        "WHERE deleted_at IS NULL AND "
        "((owner_user_id=? AND shared_with_user_id=?) OR (owner_user_id=? AND shared_with_user_id=?));",
        {now, as, bs, bs, as});
}

bool CircleService::in_circle_together(std::int64_t a, std::int64_t b) {
    db::Result r = db_.execute(
        "SELECT 1 FROM care_circle_members x "
        "JOIN care_circle_members y ON x.care_circle_id=y.care_circle_id "
        "WHERE x.user_id=? AND y.user_id=? AND x.status=? AND y.status=? "
        "AND x.deleted_at IS NULL AND y.deleted_at IS NULL LIMIT 1;",
        {std::to_string(a), std::to_string(b), kAccepted, kAccepted});
    return !r.rows.empty() && !r.rows[0].empty();
}

bool CircleService::resolve_member(std::int64_t member_id, std::int64_t* circle_id,
                                   std::int64_t* user_id) {
    if (member_id <= 0) return false;
    db::Result r = db_.execute(
        "SELECT care_circle_id, user_id FROM care_circle_members WHERE id=? AND deleted_at IS NULL;",
        {std::to_string(member_id)});
    if (r.rows.empty() || r.rows[0].size() < 2 || r.rows[0][0].is_null() || r.rows[0][1].is_null())
        return false;
    if (circle_id) *circle_id = r.rows[0][0].as_int();
    if (user_id)   *user_id   = r.rows[0][1].as_int();
    return true;
}

bool CircleService::owns_conversation(std::int64_t owner_id, const std::string& conversation_id) {
    db::Result r = db_.execute(
        "SELECT 1 FROM conversations WHERE id=? AND user_id=? AND deleted_at IS NULL;",
        {conversation_id, std::to_string(owner_id)});
    return !r.rows.empty() && !r.rows[0].empty();
}

void CircleService::send_invite_email(const std::string& to_email, const std::string& inviter_email,
                                      const std::string& token) {
    // The validator's transport selection (SMTP / Mandrill) keyed off the same
    // EmailConfig the user service uses; nothing is stored (one-off send).
    user::EmailValidatorOptions eo;
    eo.smtp_host         = cfg_.email.smtp_host;
    eo.smtp_port         = cfg_.email.smtp_port;
    eo.smtp_user         = cfg_.email.smtp_user.empty() ? cfg_.email.from_email : cfg_.email.smtp_user;
    eo.smtp_pass         = cfg_.email.smtp_pass;
    eo.mandrill_api_key  = cfg_.email.smtp_pass;   // smtp_pass doubles as the API key
    eo.mandrill_template = cfg_.email.template_name;
    eo.from_email        = cfg_.email.from_email;
    eo.from_name         = cfg_.email.from_name;

    // Public origin for the accept link: PUBLIC_BASE_URL, else the OAuth issuer.
    std::string base = cfg_.public_base_url.empty() ? cfg_.oauth.issuer : cfg_.public_base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();

    const std::string who = inviter_email.empty() ? std::string("Someone") : inviter_email;
    std::string html =
        "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"></head><body>"
        "<div style=\"max-width:600px;margin:0 auto;padding:20px;font-family:Arial,sans-serif;\">"
        "<h2 style=\"color:#2c4a73;\">You've been invited to a care circle</h2>"
        "<p><strong>" + who + "</strong> invited you to their care circle on Mirobody, "
        "so you can share conversations and health updates.</p>";
    if (!base.empty()) {
        // Query on the app root (served as index.html), the same shape as the
        // OAuth/EHR return links -- the web client consumes ?circle_token=.
        const std::string link = base + "/?circle_token=" + token;
        html +=
            "<div style=\"text-align:center;margin:24px 0;\">"
            "<a href=\"" + link + "\" style=\"background:#4072b8;color:#fff;text-decoration:none;"
            "padding:12px 24px;border-radius:8px;font-weight:bold;\">Accept invitation</a></div>"
            "<p style=\"color:#999;font-size:12px;word-break:break-all;\">Or open this link: " + link + "</p>";
    } else {
        html += "<p>Open the Mirobody app and accept the invitation from your care-circle list.</p>";
    }
    html += "<p style=\"color:#999;font-size:12px;\">If you didn't expect this, you can ignore this email.</p>"
            "</div></body></html>";

    mirobody::optional<std::string> err =
        user::send_email(eo, to_email, "You've been invited to a care circle", html);
    if (err) platform::log_warn("circle: invite email to %s not sent: %s", to_email.c_str(), err->c_str());
    else     platform::log_info("circle: invite email sent to %s", to_email.c_str());
}

//------------------------------------------------------------------------------
// Care-circle graph
//------------------------------------------------------------------------------

void CircleService::handle_create(const server::Request& req, server::Response& res) {
    const std::int64_t uid = req.user_id;
    if (uid <= 0) { res.error(-1, "not authenticated"); return; }
    std::string name = trim_ws(body_str(req.body, "name"));
    if (name.empty())        name = "My circle";
    if (name.size() > 256)   name = name.substr(0, 256);

    try {
        // Cap owned circles per user when CIRCLE_MAX_PER_USER > 0.
        if (cfg_.circle.max_circles_per_user > 0 &&
            owned_circle_count(uid) >= cfg_.circle.max_circles_per_user) {
            res.error(-4, "you have reached the maximum number of circles");
            return;
        }
        const std::int64_t cid = create_circle(uid, name);
        if (cid <= 0) { res.error(-2, "could not create circle"); return; }
        rapidjson::Document d; d.SetObject();
        rapidjson::Document::AllocatorType& a = d.GetAllocator();
        d.AddMember("circle_id", cid, a);
        d.AddMember("name", rapidjson::Value(name.c_str(), static_cast<rapidjson::SizeType>(name.size()), a), a);
        ok_json(res, d);
    } catch (const std::exception& e) {
        platform::log_warn("circle: create failed: %s", e.what());
        res.error(-3, "could not create circle");
    }
}

void CircleService::handle_rename(const server::Request& req, server::Response& res) {
    const std::int64_t uid = req.user_id;
    if (uid <= 0) { res.error(-1, "not authenticated"); return; }
    const std::string cidStr = body_str(req.body, "care_circle_id");
    std::string name = trim_ws(body_str(req.body, "name"));
    if (cidStr.empty())      { res.error(-2, "missing care_circle_id"); return; }
    if (name.empty())        { res.error(-3, "a circle name is required"); return; }
    if (name.size() > 256)   name = name.substr(0, 256);
    const std::int64_t cid = std::strtoll(cidStr.c_str(), nullptr, 10);

    try {
        if (!owns_circle(uid, cid)) { res.error(-4, "not your circle"); return; }
        db_.execute(
            "UPDATE care_circles SET name=?, updated_at=? "
            "WHERE id=? AND owner_user_id=? AND deleted_at IS NULL;",
            {name, platform::now_unix_ms(), cidStr, std::to_string(uid)});
        res.ok();
    } catch (const std::exception& e) {
        platform::log_warn("circle: rename failed: %s", e.what());
        res.error(-5, "rename failed");
    }
}

void CircleService::handle_delete(const server::Request& req, server::Response& res) {
    const std::int64_t uid = req.user_id;
    if (uid <= 0) { res.error(-1, "not authenticated"); return; }
    const std::string cidStr = body_str(req.body, "care_circle_id");
    if (cidStr.empty()) { res.error(-2, "missing care_circle_id"); return; }
    const std::int64_t cid = std::strtoll(cidStr.c_str(), nullptr, 10);

    try {
        if (!owns_circle(uid, cid)) { res.error(-3, "not your circle"); return; }
        const std::int64_t now = platform::now_unix_ms();

        // The members (besides me) whose shares might be orphaned once this circle
        // is gone -- gathered before the soft-delete so in_circle_together below
        // reflects only the *remaining* circles.
        db::Result mem = db_.execute(
            "SELECT user_id FROM care_circle_members "
            "WHERE care_circle_id=? AND user_id<>? AND deleted_at IS NULL;",
            {cidStr, std::to_string(uid)});
        std::vector<std::int64_t> others;
        for (std::size_t i = 0; i < mem.rows.size(); ++i)
            if (!mem.rows[i].empty() && !mem.rows[i][0].is_null()) others.push_back(mem.rows[i][0].as_int());

        db_.execute(
            "UPDATE care_circle_members SET deleted_at=? WHERE care_circle_id=? AND deleted_at IS NULL;",
            {now, cidStr});
        db_.execute(
            "UPDATE care_circles SET deleted_at=? WHERE id=? AND owner_user_id=? AND deleted_at IS NULL;",
            {now, cidStr, std::to_string(uid)});
        // Drop shares only with members who are now in no remaining circle with me.
        for (std::size_t i = 0; i < others.size(); ++i)
            if (!in_circle_together(uid, others[i])) revoke_shares_between(uid, others[i], now);
        res.ok();
    } catch (const std::exception& e) {
        platform::log_warn("circle: delete failed: %s", e.what());
        res.error(-4, "delete failed");
    }
}

void CircleService::handle_invite(const server::Request& req, server::Response& res) {
    const std::int64_t uid = req.user_id;
    if (uid <= 0) { res.error(-1, "not authenticated"); return; }

    const std::string email = norm_email(body_str(req.body, "email"));
    if (email.empty() || email.find('@') == std::string::npos) {
        res.error(-2, "a valid email is required");
        return;
    }

    // Throttle before resolve_or_create_user (which creates a shell account) and
    // the email send, so a caller can't mass-create user rows or spray invite
    // mail. incr() returns the post-increment count; stamp the window TTL on the
    // first hit. Fail open if the cache backend errors (incr -> nullopt).
    // CIRCLE_INVITE_MAX <= 0 disables the rate limit.
    if (cfg_.circle.invite_max_per_window > 0) {
        const int window = cfg_.circle.invite_window_seconds > 0
                               ? cfg_.circle.invite_window_seconds : 3600;
        const std::string rk = "circle:invite:" + std::to_string(uid);
        mirobody::optional<std::int64_t> n = cache_.incr(rk);
        if (n) {
            if (*n == 1) cache_.set(rk, "1", std::chrono::seconds(window));
            if (*n > cfg_.circle.invite_max_per_window) {
                res.error(-8, "too many invitations; please try again later");
                return;
            }
        }
    }

    const std::int64_t member = resolve_or_create_user(email);
    if (member <= 0)      { res.error(-3, "could not resolve that email"); return; }
    if (member == uid)    { res.error(-4, "you are already in your own circle"); return; }

    try {
        // Target a specific circle when given (must be the caller's); otherwise
        // the default circle, created on demand for the simple single-circle flow.
        const std::string want = body_str(req.body, "care_circle_id");
        std::int64_t cid = 0;
        if (!want.empty()) {
            const std::int64_t wid = std::strtoll(want.c_str(), nullptr, 10);
            if (!can_admin_circle(uid, wid)) { res.error(-5, "not allowed to invite to this circle"); return; }
            cid = wid;
        } else {
            cid = ensure_owned_circle(uid);   // simple flow: your own default circle
        }
        if (cid <= 0) { res.error(-5, "could not open your circle"); return; }

        const std::string  cstr = std::to_string(cid);
        const std::string  mstr = std::to_string(member);
        const std::int64_t now  = platform::now_unix_ms();
        const std::string  token = random_token();

        db::Result ex = db_.execute(
            "SELECT status FROM care_circle_members "
            "WHERE care_circle_id=? AND user_id=? AND deleted_at IS NULL;",
            {cid, mstr});
        if (!ex.rows.empty() && !ex.rows[0].empty()) {
            const int status = static_cast<int>(ex.rows[0][0].as_int());
            if (status == kAccepted) {
                res.error(-6, "already in your circle");
                return;
            }
            // Pending or Declined: refresh the invite (new token, back to Pending).
            // A re-invite reuses the existing row, so it never grows the circle
            // and is exempt from the member cap below.
            db_.execute(
                "UPDATE care_circle_members SET status=?, invite_token=?, member_email=?, updated_at=? "
                "WHERE care_circle_id=? AND user_id=? AND deleted_at IS NULL;",
                {kPending, token, email, now, cid, mstr});
        } else {
            // A genuinely new member: enforce the per-circle size cap when
            // CIRCLE_MAX_MEMBERS > 0 (counts pending invites too, so a circle
            // can't be filled up with outstanding invitations).
            if (cfg_.circle.max_members_per_circle > 0 &&
                circle_member_count(cid) >= cfg_.circle.max_members_per_circle) {
                res.error(-9, "this circle is full");
                return;
            }
            db_.execute(
                "INSERT INTO care_circle_members "
                "(care_circle_id, user_id, role, status, member_email, invite_token, created_at) "
                "VALUES (?, ?, ?, ?, ?, ?, ?);",
                {cid, mstr, kMember, kPending, email, token, now});
        }
        // Email the invite link (best-effort; the invitee can also accept in-app
        // from GET /api/circle/members regardless of whether the email lands).
        send_invite_email(email, req.email, token);

        rapidjson::Document d; d.SetObject();
        d.AddMember("status", rapidjson::StringRef("pending"), d.GetAllocator());
        ok_json(res, d);
    } catch (const std::exception& e) {
        platform::log_warn("circle: invite failed: %s", e.what());
        res.error(-7, "invite failed");
    }
}

void CircleService::handle_members(const server::Request& req, server::Response& res) {
    const std::int64_t uid = req.user_id;
    if (uid <= 0) { res.error(-1, "not authenticated"); return; }
    const std::string ustr = std::to_string(uid);

    rapidjson::Document d; d.SetObject();
    rapidjson::Document::AllocatorType& a = d.GetAllocator();
    auto addstr = [&a](rapidjson::Value& obj, const char* k, const std::string& v) {
        obj.AddMember(rapidjson::StringRef(k),
                      rapidjson::Value(v.c_str(), static_cast<rapidjson::SizeType>(v.size()), a), a);
    };

    try {
        // Every circle I belong to (any role), each with its full member list.
        // my_role and my_health_access are my own membership's values, so the
        // client can gate controls and show my health level without guessing.
        rapidjson::Value circles(rapidjson::kArrayType);
        db::Result cs = db_.execute(
            "SELECT c.id, c.name, m.role, m.health_access FROM care_circles c "
            "JOIN care_circle_members m ON m.care_circle_id=c.id "
            "WHERE m.user_id=? AND m.status=? "
            "AND c.deleted_at IS NULL AND m.deleted_at IS NULL ORDER BY c.id ASC;",
            {ustr, kAccepted});
        for (std::size_t ci = 0; ci < cs.rows.size(); ++ci) {
            if (cs.rows[ci].empty() || cs.rows[ci][0].is_null()) continue;
            const std::int64_t cid = cs.rows[ci][0].as_int();
            rapidjson::Value circle(rapidjson::kObjectType);
            circle.AddMember("circle_id", cid, a);
            addstr(circle, "name", cs.rows[ci][1].is_null() ? std::string() : cs.rows[ci][1].as_text());
            circle.AddMember("my_role", static_cast<int>(cs.rows[ci][2].is_null() ? 0 : cs.rows[ci][2].as_int()), a);
            circle.AddMember("my_health_access", static_cast<int>(cs.rows[ci][3].is_null() ? 0 : cs.rows[ci][3].as_int()), a);

            rapidjson::Value members(rapidjson::kArrayType);
            // m.id is the opaque member handle the client uses for nickname /
            // role / remove / share; m.user_id stays server-side (only the `me`
            // flag is derived from it) so the users PK never reaches the client.
            db::Result r = db_.execute(
                "SELECT m.id, m.user_id, u.email, m.member_email, m.status, m.role, m.health_access, m.nickname "
                "FROM care_circle_members m LEFT JOIN users u ON u.id=m.user_id "
                "WHERE m.care_circle_id=? AND m.deleted_at IS NULL ORDER BY m.created_at ASC;",
                {std::to_string(cid)});
            for (std::size_t i = 0; i < r.rows.size(); ++i) {
                const std::vector<db::Value>& row = r.rows[i];
                const std::string email = !row[2].is_null() ? row[2].as_text()
                                        : (!row[3].is_null() ? row[3].as_text() : std::string());
                rapidjson::Value item(rapidjson::kObjectType);
                item.AddMember("member", row[0].is_null() ? 0 : row[0].as_int(), a);
                addstr(item, "email", email);
                addstr(item, "status", std::string(status_str(static_cast<int>(row[4].is_null() ? 0 : row[4].as_int()))));
                item.AddMember("role", static_cast<int>(row[5].is_null() ? 0 : row[5].as_int()), a);
                item.AddMember("health_access", static_cast<int>(row[6].is_null() ? 0 : row[6].as_int()), a);
                addstr(item, "nickname", row[7].is_null() ? std::string() : row[7].as_text());
                item.AddMember("me", !row[1].is_null() && row[1].as_int() == uid, a);
                members.PushBack(item, a);
            }
            circle.AddMember("members", members, a);
            circles.PushBack(circle, a);
        }
        d.AddMember("circles", circles, a);

        // Pending invites *to me* (someone else's circle), so I can accept in-app.
        // Identify the inviter by email/circle name only -- the owner's users PK
        // is never sent.
        rapidjson::Value invites(rapidjson::kArrayType);
        db::Result iv = db_.execute(
            "SELECT m.care_circle_id, m.invite_token, ou.email, c.name "
            "FROM care_circle_members m JOIN care_circles c ON c.id=m.care_circle_id "
            "LEFT JOIN users ou ON ou.id=c.owner_user_id "
            "WHERE m.user_id=? AND m.status=? AND m.deleted_at IS NULL AND c.deleted_at IS NULL "
            "ORDER BY m.created_at ASC;",
            {ustr, kPending});
        for (std::size_t i = 0; i < iv.rows.size(); ++i) {
            const std::vector<db::Value>& row = iv.rows[i];
            rapidjson::Value item(rapidjson::kObjectType);
            item.AddMember("circle_id", row[0].is_null() ? 0 : row[0].as_int(), a);
            addstr(item, "token", row[1].is_null() ? std::string() : row[1].as_text());
            addstr(item, "owner_email", row[2].is_null() ? std::string() : row[2].as_text());
            addstr(item, "circle_name", row[3].is_null() ? std::string() : row[3].as_text());
            invites.PushBack(item, a);
        }
        d.AddMember("invites", invites, a);

        ok_json(res, d);
    } catch (const std::exception& e) {
        platform::log_warn("circle: members query failed: %s", e.what());
        res.error(-2, "members query failed");
    }
}

void CircleService::handle_accept(const server::Request& req, server::Response& res) {
    const std::int64_t uid = req.user_id;
    if (uid <= 0) { res.error(-1, "not authenticated"); return; }
    const std::string token = body_str(req.body, "token");
    if (token.empty()) { res.error(-2, "missing invite token"); return; }

    try {
        // Only the invitee (a pending membership with this token) can accept.
        db::Result sel = db_.execute(
            "SELECT id FROM care_circle_members "
            "WHERE user_id=? AND invite_token=? AND status=? AND deleted_at IS NULL;",
            {std::to_string(uid), token, kPending});
        if (sel.rows.empty() || sel.rows[0].empty()) {
            res.error(-3, "invalid or expired invite");
            return;
        }
        // Consume the invite token on accept: it has served its purpose and
        // shouldn't linger in the row (or remain replayable from a leaked link).
        db_.execute("UPDATE care_circle_members SET status=?, invite_token=NULL, updated_at=? WHERE id=?;",
                    {kAccepted, platform::now_unix_ms(), sel.rows[0][0].as_int()});
        res.ok();
    } catch (const std::exception& e) {
        platform::log_warn("circle: accept failed: %s", e.what());
        res.error(-4, "accept failed");
    }
}

void CircleService::handle_decline(const server::Request& req, server::Response& res) {
    const std::int64_t uid = req.user_id;
    if (uid <= 0) { res.error(-1, "not authenticated"); return; }
    const std::string token = body_str(req.body, "token");
    if (token.empty()) { res.error(-2, "missing invite token"); return; }

    try {
        db::Result sel = db_.execute(
            "SELECT id FROM care_circle_members "
            "WHERE user_id=? AND invite_token=? AND status=? AND deleted_at IS NULL;",
            {std::to_string(uid), token, kPending});
        if (sel.rows.empty() || sel.rows[0].empty()) {
            res.error(-3, "invalid or expired invite");
            return;
        }
        db_.execute("UPDATE care_circle_members SET status=?, invite_token=NULL, updated_at=? WHERE id=?;",
                    {kDeclined, platform::now_unix_ms(), sel.rows[0][0].as_int()});
        res.ok();
    } catch (const std::exception& e) {
        platform::log_warn("circle: decline failed: %s", e.what());
        res.error(-4, "decline failed");
    }
}

void CircleService::handle_remove(const server::Request& req, server::Response& res) {
    const std::int64_t uid = req.user_id;
    if (uid <= 0) { res.error(-1, "not authenticated"); return; }
    const std::vector<std::int64_t> handles = body_handles(req.body);
    if (handles.empty()) { res.error(-2, "missing member"); return; }

    try {
        // The handle carries its own circle + user, so neither is taken from the
        // client as a raw id.
        std::int64_t cid = 0, member = 0;
        if (!resolve_member(handles[0], &cid, &member)) { res.error(-3, "unknown member"); return; }

        // Admins (Owner / Maintainer) may remove. An owner can remove anyone but
        // the owner row; a maintainer may remove plain Members only -- not the
        // owner or fellow maintainers.
        const int myRole     = member_role(uid, cid);
        const int targetRole = member_role(member, cid);
        if (myRole < kMaintainer)         { res.error(-3, "not allowed to manage this circle"); return; }
        if (targetRole == kOwner)         { res.error(-5, "cannot remove the circle owner"); return; }
        if (myRole < kOwner && targetRole >= kMaintainer) {
            res.error(-6, "maintainers can only remove members");
            return;
        }

        const std::string  mstr = std::to_string(member);
        const std::int64_t now  = platform::now_unix_ms();

        db_.execute(
            "UPDATE care_circle_members SET deleted_at=? "
            "WHERE care_circle_id=? AND user_id=? AND deleted_at IS NULL;",
            {now, std::to_string(cid), mstr});
        // Revoke shares only if the two no longer co-inhabit any circle -- a share
        // row outlives circle membership otherwise, since the read path checks the
        // share directly, but it must survive removal from just one shared circle.
        if (!in_circle_together(uid, member)) revoke_shares_between(uid, member, now);
        res.ok();
    } catch (const std::exception& e) {
        platform::log_warn("circle: remove failed: %s", e.what());
        res.error(-4, "remove failed");
    }
}

void CircleService::handle_set_nickname(const server::Request& req, server::Response& res) {
    const std::int64_t uid = req.user_id;
    if (uid <= 0) { res.error(-1, "not authenticated"); return; }
    const std::vector<std::int64_t> handles = body_handles(req.body);
    if (handles.empty()) { res.error(-2, "missing member"); return; }
    std::string nick = trim_ws(body_str(req.body, "nickname"));   // "" clears it
    if (nick.size() > 256) nick = nick.substr(0, 256);

    try {
        std::int64_t cid = 0, member = 0;
        if (!resolve_member(handles[0], &cid, &member)) { res.error(-2, "unknown member"); return; }
        if (!can_admin_circle(uid, cid)) { res.error(-3, "not allowed to manage this circle"); return; }
        db_.execute(
            "UPDATE care_circle_members SET nickname=?, updated_at=? "
            "WHERE care_circle_id=? AND user_id=? AND deleted_at IS NULL;",
            {nick, platform::now_unix_ms(), std::to_string(cid), std::to_string(member)});
        res.ok();
    } catch (const std::exception& e) {
        platform::log_warn("circle: set nickname failed: %s", e.what());
        res.error(-4, "could not update nickname");
    }
}

void CircleService::handle_set_role(const server::Request& req, server::Response& res) {
    const std::int64_t uid = req.user_id;
    if (uid <= 0) { res.error(-1, "not authenticated"); return; }
    const std::vector<std::int64_t> handles = body_handles(req.body);
    if (handles.empty()) { res.error(-2, "missing member"); return; }

    // Target role: Member or Maintainer only (Owner is never assignable here).
    const std::string rstr = body_str(req.body, "role");
    int role = kMember;
    if (rstr == "maintainer" || rstr == std::to_string(kMaintainer)) role = kMaintainer;

    try {
        std::int64_t cid = 0, member = 0;
        if (!resolve_member(handles[0], &cid, &member)) { res.error(-2, "unknown member"); return; }
        // Owner-only: only the circle owner promotes/demotes, and never their own
        // row nor another owner (the `role<>Owner` guard).
        if (!owns_circle(uid, cid))  { res.error(-3, "only the owner can change roles"); return; }
        if (member == uid)           { res.error(-4, "you cannot change your own role"); return; }
        db_.execute(
            "UPDATE care_circle_members SET role=?, updated_at=? "
            "WHERE care_circle_id=? AND user_id=? AND role<>? AND deleted_at IS NULL;",
            {role, platform::now_unix_ms(), std::to_string(cid), std::to_string(member), kOwner});
        res.ok();
    } catch (const std::exception& e) {
        platform::log_warn("circle: set role failed: %s", e.what());
        res.error(-5, "could not update role");
    }
}

void CircleService::handle_health_sharing(const server::Request& req, server::Response& res) {
    const std::int64_t uid = req.user_id;
    if (uid <= 0) { res.error(-1, "not authenticated"); return; }

    // Preferred: {access: "off"|"view"|"edit"} (or the numeric ShareAccess 0/1/2).
    // Back-compat: {enabled: bool} -> View / Off. Anything unrecognized -> Off.
    int level = 0;   // ShareAccess: 0=off, kView=read, kEdit=read+write
    rapidjson::Document d;
    if (!d.Parse(req.body.c_str()).HasParseError() && d.IsObject()) {
        rapidjson::Value::ConstMemberIterator acc = d.FindMember("access");
        rapidjson::Value::ConstMemberIterator en  = d.FindMember("enabled");
        if (acc != d.MemberEnd()) {
            if (acc->value.IsString()) {
                const std::string s = acc->value.GetString();
                if      (s == "edit") level = kEdit;
                else if (s == "view") level = kView;
                else                  level = 0;          // "off" / "none" / unknown
            } else if (acc->value.IsInt()) {
                const int v = acc->value.GetInt();
                level = (v >= kEdit) ? kEdit : (v >= kView ? kView : 0);
            }
        } else if (en != d.MemberEnd()) {
            bool enabled = false;
            if (en->value.IsBool())        enabled = en->value.GetBool();
            else if (en->value.IsInt())    enabled = en->value.GetInt() != 0;
            else if (en->value.IsString()) { const std::string s = en->value.GetString(); enabled = (s == "true" || s == "1"); }
            level = enabled ? kView : 0;
        }
    }
    const std::int64_t now  = platform::now_unix_ms();
    const std::string  ustr = std::to_string(uid);
    const std::string  cid  = body_str(req.body, "care_circle_id");

    // Health sharing is per circle. Requiring care_circle_id keeps the scope
    // explicit: a caller in several circles (e.g. Family and Work) can't flip
    // them all open at once by omitting it — each circle is set on its own.
    if (cid.empty()) { res.error(-3, "care_circle_id is required"); return; }
    const std::int64_t cidNum = std::strtoll(cid.c_str(), nullptr, 10);

    try {
        if (member_role(uid, cidNum) < kMember) {
            res.error(-4, "not a member of that circle");
            return;
        }
        db_.execute(
            "UPDATE care_circle_members SET health_access=?, updated_at=? "
            "WHERE user_id=? AND care_circle_id=? AND deleted_at IS NULL;",
            {level, now, ustr, cid});
        rapidjson::Document out; out.SetObject();
        out.AddMember("access", rapidjson::StringRef(level == kEdit ? "edit" : (level == kView ? "view" : "off")),
                      out.GetAllocator());
        ok_json(res, out);
    } catch (const std::exception& e) {
        platform::log_warn("circle: health-sharing update failed: %s", e.what());
        res.error(-2, "could not update health sharing");
    }
}

void CircleService::handle_health_shared_with_me(const server::Request& req, server::Response& res) {
    const std::int64_t uid = req.user_id;
    if (uid <= 0) { res.error(-1, "not authenticated"); return; }
    try {
        std::vector<HealthShare> people = health_shared_with(db_, uid);
        rapidjson::Document d; d.SetObject();
        rapidjson::Document::AllocatorType& a = d.GetAllocator();
        rapidjson::Value users(rapidjson::kArrayType);
        for (std::size_t i = 0; i < people.size(); ++i) {
            rapidjson::Value item(rapidjson::kObjectType);
            item.AddMember("member", people[i].member_id, a);   // opaque handle, not the users PK
            item.AddMember("email", rapidjson::Value(people[i].email.c_str(),
                           static_cast<rapidjson::SizeType>(people[i].email.size()), a), a);
            item.AddMember("nickname", rapidjson::Value(people[i].nickname.c_str(),
                           static_cast<rapidjson::SizeType>(people[i].nickname.size()), a), a);
            users.PushBack(item, a);
        }
        d.AddMember("users", users, a);
        ok_json(res, d);
    } catch (const std::exception& e) {
        platform::log_warn("circle: health-shared-with-me failed: %s", e.what());
        res.error(-2, "could not list shared users");
    }
}

//------------------------------------------------------------------------------
// Health-data read authorization (declared in circle/access.hpp)
//------------------------------------------------------------------------------

bool can_read_health(database::Database& conn, std::int64_t viewer, std::int64_t target) {
    if (viewer <= 0) return false;
    if (viewer == target) return true;   // you always read your own
#if defined(MIROBODY_DATABASE_PG_LEGACY)
    (void)conn;
    return false;                        // no care circles on the legacy backend
#else
    return circle_health_grants(conn, viewer, target, kView);
#endif
}

bool can_write_health(database::Database& conn, std::int64_t viewer, std::int64_t target) {
    if (viewer <= 0) return false;
    if (viewer == target) return true;   // you always write your own
#if defined(MIROBODY_DATABASE_PG_LEGACY)
    (void)conn;
    return false;
#else
    return circle_health_grants(conn, viewer, target, kEdit);
#endif
}

std::int64_t resolve_health_subject(database::Database& conn, std::int64_t viewer,
                                    std::int64_t member_id, bool need_write) {
    if (viewer <= 0 || member_id <= 0) return 0;
#if defined(MIROBODY_DATABASE_PG_LEGACY)
    (void)conn; (void)need_write;
    return 0;                            // no care circles on the legacy backend
#else
    try {
        // The handle names one membership row (owner); authorize the viewer
        // through that SAME circle: both Accepted, and the owner's health_access
        // meets the needed level. Returns the owner's user_id only when allowed.
        database::Result r = conn.execute(
            "SELECT owner.user_id FROM care_circle_members owner "
            "JOIN care_circle_members me ON me.care_circle_id=owner.care_circle_id "
            "WHERE owner.id=? AND me.user_id=? AND owner.status=? AND me.status=? "
            "AND owner.health_access >= ? AND owner.deleted_at IS NULL AND me.deleted_at IS NULL "
            "LIMIT 1;",
            {std::to_string(member_id), std::to_string(viewer),
             kAccepted, kAccepted, need_write ? kEdit : kView});
        if (r.rows.empty() || r.rows[0].empty() || r.rows[0][0].is_null()) return 0;
        return r.rows[0][0].as_int();
    } catch (const std::exception&) {
        return 0;
    }
#endif
}

std::vector<HealthShare> health_shared_with(database::Database& conn, std::int64_t viewer) {
    std::vector<HealthShare> out;
    if (viewer <= 0) return out;
#if defined(MIROBODY_DATABASE_PG_LEGACY)
    (void)conn;
    return out;
#else
    try {
        const std::string v = std::to_string(viewer);
        // MAX(owner.id) yields one stable membership-row handle per sharer (a
        // person sharing through several circles collapses to one entry); that
        // handle round-trips through resolve_health_subject without ever exposing
        // the sharer's users PK.
        database::Result r = conn.execute(
            "SELECT owner.user_id, MAX(u.email), MAX(owner.nickname), MAX(owner.id) "
            "FROM care_circle_members me "
            "JOIN care_circle_members owner ON me.care_circle_id=owner.care_circle_id "
            "LEFT JOIN users u ON u.id=owner.user_id "
            "WHERE me.user_id=? AND owner.user_id<>? AND me.status=? AND owner.status=? "
            "AND owner.health_access>=? AND me.deleted_at IS NULL AND owner.deleted_at IS NULL "
            "GROUP BY owner.user_id;",
            {v, v, kAccepted, kAccepted, kView});
        for (std::size_t i = 0; i < r.rows.size(); ++i) {
            const std::vector<database::Value>& row = r.rows[i];
            if (row.empty() || row[0].is_null()) continue;
            HealthShare h;
            h.user_id   = row[0].as_int();
            h.email     = (row.size() > 1 && !row[1].is_null()) ? row[1].as_text() : std::string();
            h.nickname  = (row.size() > 2 && !row[2].is_null()) ? row[2].as_text() : std::string();
            h.member_id = (row.size() > 3 && !row[3].is_null()) ? row[3].as_int()  : 0;
            out.push_back(h);
        }
    } catch (const std::exception&) {
    }
    return out;
#endif
}

//------------------------------------------------------------------------------
// Conversation sharing
//------------------------------------------------------------------------------

void CircleService::handle_share(const server::Request& req, server::Response& res) {
    const std::int64_t uid = req.user_id;
    if (uid <= 0) { res.error(-1, "not authenticated"); return; }

    const std::string cid = body_str(req.body, "conversation_id");
    if (cid.empty()) { res.error(-2, "missing conversation_id"); return; }
    const std::vector<std::int64_t> handles = body_handles(req.body);
    if (handles.empty()) { res.error(-3, "no members to share with"); return; }
    const int access = (body_str(req.body, "access") == "edit") ? kEdit : kView;

    try {
        if (!owns_conversation(uid, cid)) { res.error(-4, "not your conversation"); return; }

        const std::int64_t now = platform::now_unix_ms();
        int shared = 0;
        for (std::size_t i = 0; i < handles.size(); ++i) {
            std::int64_t mcid = 0, m = 0;
            // Resolve the handle to a user, then require a real shared circle.
            if (!resolve_member(handles[i], &mcid, &m)) continue;
            if (m == uid || !in_circle_together(uid, m)) continue;   // only circle members
            const std::string mstr = std::to_string(m);
            db::Result ex = db_.execute(
                "SELECT id FROM conversation_shares "
                "WHERE conversation_id=? AND shared_with_user_id=? AND deleted_at IS NULL;",
                {cid, mstr});
            if (!ex.rows.empty() && !ex.rows[0].empty()) {
                db_.execute("UPDATE conversation_shares SET access_level=? WHERE id=?;",
                            {access, ex.rows[0][0].as_int()});
            } else {
                db_.execute(
                    "INSERT INTO conversation_shares "
                    "(conversation_id, owner_user_id, shared_with_user_id, access_level, created_at) "
                    "VALUES (?, ?, ?, ?, ?);",
                    {cid, std::to_string(uid), mstr, access, now});
            }
            ++shared;
        }
        rapidjson::Document d; d.SetObject();
        d.AddMember("shared", shared, d.GetAllocator());
        ok_json(res, d);
    } catch (const std::exception& e) {
        platform::log_warn("circle: share failed: %s", e.what());
        res.error(-5, "share failed");
    }
}

void CircleService::handle_unshare(const server::Request& req, server::Response& res) {
    const std::int64_t uid = req.user_id;
    if (uid <= 0) { res.error(-1, "not authenticated"); return; }
    const std::string cid = body_str(req.body, "conversation_id");
    const std::vector<std::int64_t> handles = body_handles(req.body);
    if (cid.empty() || handles.empty()) { res.error(-2, "missing conversation_id / member"); return; }

    try {
        if (!owns_conversation(uid, cid)) { res.error(-3, "not your conversation"); return; }
        std::int64_t mcid = 0, m = 0;
        if (!resolve_member(handles[0], &mcid, &m)) { res.error(-3, "unknown member"); return; }
        db_.execute(
            "UPDATE conversation_shares SET deleted_at=? "
            "WHERE conversation_id=? AND shared_with_user_id=? AND owner_user_id=? AND deleted_at IS NULL;",
            {platform::now_unix_ms(), cid, std::to_string(m), std::to_string(uid)});
        res.ok();
    } catch (const std::exception& e) {
        platform::log_warn("circle: unshare failed: %s", e.what());
        res.error(-4, "unshare failed");
    }
}

void CircleService::handle_shares(const server::Request& req, server::Response& res) {
    const std::int64_t uid = req.user_id;
    if (uid <= 0) { res.error(-1, "not authenticated"); return; }
    std::string cid = req.query_str("id", "");
    if (cid.empty()) cid = body_str(req.body, "id");
    if (cid.empty()) { res.error(-2, "missing conversation id"); return; }

    try {
        if (!owns_conversation(uid, cid)) { res.error(-3, "not your conversation"); return; }
        // Recipients are identified to the client by email only (the matching
        // picker keys on it); the shared_with users PK is not serialized.
        db::Result r = db_.execute(
            "SELECT s.access_level, u.email "
            "FROM conversation_shares s LEFT JOIN users u ON u.id=s.shared_with_user_id "
            "WHERE s.conversation_id=? AND s.deleted_at IS NULL ORDER BY s.created_at ASC;",
            {cid});

        rapidjson::Document d; d.SetObject();
        rapidjson::Document::AllocatorType& a = d.GetAllocator();
        rapidjson::Value shares(rapidjson::kArrayType);
        for (std::size_t i = 0; i < r.rows.size(); ++i) {
            const std::vector<db::Value>& row = r.rows[i];
            const int lvl = static_cast<int>(row[0].is_null() ? kView : row[0].as_int());
            rapidjson::Value item(rapidjson::kObjectType);
            const std::string email = row[1].is_null() ? std::string() : row[1].as_text();
            item.AddMember("email", rapidjson::Value(email.c_str(), static_cast<rapidjson::SizeType>(email.size()), a), a);
            item.AddMember("access", rapidjson::StringRef(lvl == kEdit ? "edit" : "view"), a);
            shares.PushBack(item, a);
        }
        d.AddMember("shares", shares, a);
        ok_json(res, d);
    } catch (const std::exception& e) {
        platform::log_warn("circle: shares query failed: %s", e.what());
        res.error(-4, "shares query failed");
    }
}

}}
