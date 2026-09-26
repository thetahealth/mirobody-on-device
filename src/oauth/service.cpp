#include "oauth/service.hpp"
#include <optional>

#include "oauth/pkce.hpp"
#include "platform/log.hpp"
#include "server/request.hpp"   // Request::parse_form

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <ctime>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace mirobody { namespace oauth {

namespace {

// Cache key prefixes. Mirrors the "mirobody:..." Redis convention used by the
// MCP personal-secret store (see mcp/service.cpp).
const char* const kClientPrefix = "oauth:client:";   // <id>     -> client JSON   (long TTL)
const char* const kAuthzPrefix  = "oauth:authzreq:"; // <handle> -> request JSON  (10 min)
const char* const kCodePrefix   = "oauth:code:";     // <code>   -> grant JSON    (single-use)

//------------------------------------------------------------------------------
// URL / form encoding helpers

bool is_unreserved(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (is_unreserved(static_cast<char>(c))) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

std::string form_get(const std::map<std::string, std::string>& m, const char* k) {
    std::map<std::string, std::string>::const_iterator it = m.find(k);
    return it == m.end() ? std::string() : it->second;
}

//------------------------------------------------------------------------------
// scope helpers

std::vector<std::string> split_scope(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream is(s);
    std::string tok;
    while (is >> tok) out.push_back(tok);
    return out;
}

// The requested scope narrowed to what the server supports. An empty request
// yields the full supported set (the OAuth default-scope behaviour).
std::string grant_scope(const std::string& requested, const std::string& supported) {
    const std::vector<std::string> sup = split_scope(supported);
    if (requested.empty()) return supported;
    std::set<std::string> allow(sup.begin(), sup.end());
    std::string out;
    const std::vector<std::string> req = split_scope(requested);
    for (std::size_t i = 0; i < req.size(); ++i) {
        if (allow.count(req[i])) {
            if (!out.empty()) out += ' ';
            out += req[i];
        }
    }
    return out.empty() ? supported : out;
}

//------------------------------------------------------------------------------
// rapidjson read helpers

std::string json_str(const rapidjson::Value& v, const char* key) {
    if (!v.IsObject()) return std::string();
    rapidjson::Value::ConstMemberIterator it = v.FindMember(key);
    if (it != v.MemberEnd() && it->value.IsString()) {
        return std::string(it->value.GetString(), it->value.GetStringLength());
    }
    return std::string();
}

//------------------------------------------------------------------------------
// JSON response writers

// A raw spec-shaped JSON body (NOT the project {code,msg,data} envelope), since
// OAuth clients expect exact RFC bodies. `status` sets the HTTP status.
void send_json(server::Response& res, int status, const std::string& body) {
    res.status(status);
    res.json(body);
}

// RFC 6749 sec.5.2 / RFC 7591 error body.
void send_oauth_error(server::Response& res, int status, const char* error,
                      const std::string& description) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("error");             w.String(error);
    if (!description.empty()) {
        w.Key("error_description"); w.String(description.c_str(),
                                             static_cast<rapidjson::SizeType>(description.size()));
    }
    w.EndObject();
    send_json(res, status, std::string(buf.GetString(), buf.GetSize()));
}

// Append ?code=&state= (or ?error=&state=) to a redirect URI, respecting any
// query already present, and emit a 302 to it. State is omitted when empty.
void redirect_with(server::Response& res, const std::string& redirect_uri,
                   const std::string& kv, const std::string& state) {
    std::string url = redirect_uri;
    url += (url.find('?') == std::string::npos) ? '?' : '&';
    url += kv;
    if (!state.empty()) {
        url += "&state=";
        url += url_encode(state);
    }
    res.status(302);
    res.header("Location", url);
    res.header("Cache-Control", "no-store");
    res.text("");
}

// "http" for loopback hosts (dev), "https" otherwise — mirrors mcp/service.cpp.
const char* url_scheme(const std::string& host) {
    if (host.rfind("localhost", 0) == 0 || host.rfind("127.0.0.1", 0) == 0) {
        return "http";
    }
    return "https";
}

std::int64_t now_unix() { return static_cast<std::int64_t>(std::time(nullptr)); }

}  // namespace

//------------------------------------------------------------------------------

OAuthService::OAuthService(server::Router& router, const Config& cfg,
                           cache::Cache& cache, const jwt::Jwt& jwt,
                           std::string jwks_json)
    : cfg_(cfg), cache_(cache), jwt_(jwt), uri_prefix_(cfg.uri_prefix),
      jwks_json_(std::move(jwks_json)) {
    register_routes(router);
}

void OAuthService::register_routes(server::Router& router) {
    // Discovery lives at the host root so clients locate it without knowing the
    // app's mount prefix; the endpoints it advertises carry the prefix.
    router.get_unprefixed("/.well-known/oauth-protected-resource",
        [this](const server::Request& req, server::Response& res) {
            protected_resource_metadata(req, res);
        });
    router.get_unprefixed("/.well-known/oauth-authorization-server",
        [this](const server::Request& req, server::Response& res) {
            authorization_server_metadata(req, res);
        });
    // Compatibility alias for OAuth clients that only probe OpenID Connect
    // discovery: serve the SAME RFC 8414 metadata body here. This is an OAuth
    // authorization-server document, not a full OIDC provider config (no ID
    // tokens / userinfo / jwks_uri -- tokens are HS256), but the overlapping
    // fields are what such clients consume to find the endpoints.
    router.get_unprefixed("/.well-known/openid-configuration",
        [this](const server::Request& req, server::Response& res) {
            authorization_server_metadata(req, res);
        });
    // Public JWKS for third-party validators — only when signing asymmetrically
    // (RS256). Under HS256 there is no public key, so the endpoint is omitted
    // and discovery advertises no jwks_uri.
    if (!jwks_json_.empty()) {
        router.get_unprefixed("/.well-known/jwks.json",
            [this](const server::Request&, server::Response& res) {
                send_json(res, 200, jwks_json_);
            });
    }

    router.post("/oauth/register",
        [this](const server::Request& req, server::Response& res) { register_client(req, res); });
    router.get("/oauth/authorize",
        [this](const server::Request& req, server::Response& res) { authorize(req, res); });
    router.get("/oauth/authorize/info",
        [this](const server::Request& req, server::Response& res) { authorize_info(req, res); });
    router.post("/oauth/authorize/decision",
        [this](const server::Request& req, server::Response& res) { authorize_decision(req, res); });
    router.post("/oauth/token",
        [this](const server::Request& req, server::Response& res) { token(req, res); });
    router.post("/oauth/revoke",
        [this](const server::Request& req, server::Response& res) { revoke(req, res); });
}

//------------------------------------------------------------------------------
// URL helpers

std::string OAuthService::base_url(const server::Request& req) const {
    if (!cfg_.oauth.issuer.empty()) {
        std::string b = cfg_.oauth.issuer;
        while (!b.empty() && b.back() == '/') b.pop_back();
        return b;
    }
    return std::string(url_scheme(req.host)) + "://" + req.host;
}

std::string OAuthService::endpoint(const server::Request& req, const std::string& path) const {
    return base_url(req) + uri_prefix_ + path;
}

bool OAuthService::redirect_uri_allowed(const std::string& uri) const {
    if (uri.empty()) return false;

    // Loopback http (RFC 8252) — native desktop / CLI clients on 127.0.0.1.
    if (uri.rfind("http://127.0.0.1", 0) == 0 ||
        uri.rfind("http://localhost", 0) == 0 ||
        uri.rfind("http://[::1]", 0) == 0) {
        return true;
    }
    if (uri.rfind("http://", 0) == 0) {
        return false;   // non-loopback plaintext http is never allowed
    }
    if (uri.rfind("https://", 0) == 0) {
        if (cfg_.oauth.allowed_redirect_hosts.empty()) return true;
        // Extract host between "https://" and the next '/', ':' or end.
        const std::size_t start = 8;
        std::size_t end = start;
        while (end < uri.size() && uri[end] != '/' && uri[end] != ':') ++end;
        const std::string host = uri.substr(start, end - start);
        // Accept either space- or comma-separated lists: normalize commas to
        // spaces, then tokenize on whitespace.
        std::string list = cfg_.oauth.allowed_redirect_hosts;
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (list[i] == ',') list[i] = ' ';
        }
        std::istringstream is(list);
        std::string allowed;
        while (is >> allowed) {
            if (allowed == host) return true;
        }
        return false;
    }
    // Any other scheme (e.g. "myapp://callback") is a custom-scheme native app
    // redirect; accept it. There is no host to validate.
    return uri.find("://") != std::string::npos;
}

//------------------------------------------------------------------------------
// Discovery

void OAuthService::protected_resource_metadata(const server::Request& req, server::Response& res) {
    const std::string resource = base_url(req) + uri_prefix_ + cfg_.oauth.resource_path;
    const std::string issuer   = base_url(req);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("resource"); w.String(resource.c_str(), static_cast<rapidjson::SizeType>(resource.size()));
    w.Key("authorization_servers");
    w.StartArray();
    w.String(issuer.c_str(), static_cast<rapidjson::SizeType>(issuer.size()));
    w.EndArray();
    w.Key("scopes_supported");
    w.StartArray();
    { const std::vector<std::string> sc = split_scope(cfg_.oauth.scopes_supported);
      for (std::size_t i = 0; i < sc.size(); ++i) w.String(sc[i].c_str(),
                                                  static_cast<rapidjson::SizeType>(sc[i].size())); }
    w.EndArray();
    w.Key("bearer_methods_supported");
    w.StartArray(); w.String("header"); w.EndArray();
    w.EndObject();

    send_json(res, 200, std::string(buf.GetString(), buf.GetSize()));
}

void OAuthService::authorization_server_metadata(const server::Request& req, server::Response& res) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();

    const std::string issuer = base_url(req);
    w.Key("issuer"); w.String(issuer.c_str(), static_cast<rapidjson::SizeType>(issuer.size()));

    auto put = [&](const char* k, const std::string& v) {
        w.Key(k); w.String(v.c_str(), static_cast<rapidjson::SizeType>(v.size()));
    };
    put("authorization_endpoint", endpoint(req, "/oauth/authorize"));
    put("token_endpoint",         endpoint(req, "/oauth/token"));
    put("registration_endpoint",  endpoint(req, "/oauth/register"));
    put("revocation_endpoint",    endpoint(req, "/oauth/revoke"));
    // jwks_uri is advertised only under RS256, where there is a public key to
    // publish. It lives at the host root (unprefixed), like the discovery docs.
    if (!jwks_json_.empty()) put("jwks_uri", base_url(req) + "/.well-known/jwks.json");

    w.Key("scopes_supported");
    w.StartArray();
    { const std::vector<std::string> sc = split_scope(cfg_.oauth.scopes_supported);
      for (std::size_t i = 0; i < sc.size(); ++i) w.String(sc[i].c_str(),
                                                  static_cast<rapidjson::SizeType>(sc[i].size())); }
    w.EndArray();

    w.Key("response_types_supported"); w.StartArray(); w.String("code"); w.EndArray();
    w.Key("grant_types_supported");
    w.StartArray(); w.String("authorization_code"); w.String("refresh_token"); w.EndArray();
    w.Key("code_challenge_methods_supported"); w.StartArray(); w.String("S256"); w.EndArray();
    w.Key("token_endpoint_auth_methods_supported"); w.StartArray(); w.String("none"); w.EndArray();
    w.EndObject();

    send_json(res, 200, std::string(buf.GetString(), buf.GetSize()));
}

//------------------------------------------------------------------------------
// Dynamic client registration (RFC 7591)

void OAuthService::register_client(const server::Request& req, server::Response& res) {
    rapidjson::Document doc;
    if (doc.Parse(req.body.c_str(), req.body.size()).HasParseError() || !doc.IsObject()) {
        send_oauth_error(res, 400, "invalid_client_metadata", "Request body must be a JSON object");
        return;
    }

    // redirect_uris is required and every entry must pass the policy.
    rapidjson::Value::ConstMemberIterator ru = doc.FindMember("redirect_uris");
    if (ru == doc.MemberEnd() || !ru->value.IsArray() || ru->value.Empty()) {
        send_oauth_error(res, 400, "invalid_redirect_uri", "redirect_uris is required");
        return;
    }
    std::vector<std::string> redirect_uris;
    for (rapidjson::SizeType i = 0; i < ru->value.Size(); ++i) {
        if (!ru->value[i].IsString()) {
            send_oauth_error(res, 400, "invalid_redirect_uri", "redirect_uris must be strings");
            return;
        }
        const std::string uri(ru->value[i].GetString(), ru->value[i].GetStringLength());
        if (!redirect_uri_allowed(uri)) {
            send_oauth_error(res, 400, "invalid_redirect_uri", "redirect_uri not allowed: " + uri);
            return;
        }
        redirect_uris.push_back(uri);
    }

    const std::string client_name = json_str(doc, "client_name");
    const std::string scope = json_str(doc, "scope");

    const std::string client_id = "mb_" + random_token(18);
    const std::int64_t issued_at = now_unix();

    // Persist the client record. Public client (PKCE) — no secret is issued.
    {
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        w.StartObject();
        w.Key("client_name"); w.String(client_name.c_str(),
                                       static_cast<rapidjson::SizeType>(client_name.size()));
        w.Key("scope");       w.String(scope.c_str(), static_cast<rapidjson::SizeType>(scope.size()));
        w.Key("created_at");  w.Int64(issued_at);
        w.Key("redirect_uris");
        w.StartArray();
        for (std::size_t i = 0; i < redirect_uris.size(); ++i) {
            w.String(redirect_uris[i].c_str(), static_cast<rapidjson::SizeType>(redirect_uris[i].size()));
        }
        w.EndArray();
        w.EndObject();
        cache_.set(kClientPrefix + client_id, std::string(buf.GetString(), buf.GetSize()),
                   std::chrono::seconds(cfg_.oauth.client_ttl));
    }

    // RFC 7591 sec.3.2.1 success response.
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("client_id"); w.String(client_id.c_str(), static_cast<rapidjson::SizeType>(client_id.size()));
    w.Key("client_id_issued_at"); w.Int64(issued_at);
    w.Key("token_endpoint_auth_method"); w.String("none");
    w.Key("grant_types");
    w.StartArray(); w.String("authorization_code"); w.String("refresh_token"); w.EndArray();
    w.Key("response_types"); w.StartArray(); w.String("code"); w.EndArray();
    if (!client_name.empty()) {
        w.Key("client_name"); w.String(client_name.c_str(),
                                       static_cast<rapidjson::SizeType>(client_name.size()));
    }
    if (!scope.empty()) {
        w.Key("scope"); w.String(scope.c_str(), static_cast<rapidjson::SizeType>(scope.size()));
    }
    w.Key("redirect_uris");
    w.StartArray();
    for (std::size_t i = 0; i < redirect_uris.size(); ++i) {
        w.String(redirect_uris[i].c_str(), static_cast<rapidjson::SizeType>(redirect_uris[i].size()));
    }
    w.EndArray();
    w.EndObject();

    send_json(res, 201, std::string(buf.GetString(), buf.GetSize()));
}

//------------------------------------------------------------------------------
// Authorization endpoint (RFC 6749 sec.4.1.1 + PKCE)

void OAuthService::authorize(const server::Request& req, server::Response& res) {
    const std::map<std::string, std::string> q = server::Request::parse_form(req.query);

    const std::string client_id     = form_get(q, "client_id");
    const std::string redirect_uri  = form_get(q, "redirect_uri");
    const std::string response_type = form_get(q, "response_type");
    const std::string code_challenge        = form_get(q, "code_challenge");
    const std::string code_challenge_method = form_get(q, "code_challenge_method");
    const std::string scope    = form_get(q, "scope");
    const std::string state    = form_get(q, "state");
    const std::string resource = form_get(q, "resource");

    // Resolve the client and validate the redirect_uri BEFORE any error
    // redirect: per spec, a bad client/redirect must not bounce to it.
    if (client_id.empty()) {
        send_oauth_error(res, 400, "invalid_request", "client_id is required");
        return;
    }
    std::optional<std::string> client_json = cache_.get(kClientPrefix + client_id);
    if (!client_json.has_value()) {
        send_oauth_error(res, 400, "invalid_client", "Unknown client_id");
        return;
    }
    rapidjson::Document client;
    client.Parse(client_json->c_str(), client_json->size());
    bool redirect_ok = false;
    if (client.IsObject()) {
        rapidjson::Value::ConstMemberIterator it = client.FindMember("redirect_uris");
        if (it != client.MemberEnd() && it->value.IsArray()) {
            for (rapidjson::SizeType i = 0; i < it->value.Size(); ++i) {
                if (it->value[i].IsString() &&
                    redirect_uri == std::string(it->value[i].GetString(),
                                                it->value[i].GetStringLength())) {
                    redirect_ok = true;
                    break;
                }
            }
        }
    }
    if (redirect_uri.empty() || !redirect_ok) {
        send_oauth_error(res, 400, "invalid_request",
                         "redirect_uri does not match a registered value");
        return;
    }

    // From here, protocol errors are reported by redirecting back to the client.
    if (response_type != "code") {
        redirect_with(res, redirect_uri, "error=unsupported_response_type", state);
        return;
    }
    if (code_challenge.empty() || code_challenge_method != "S256") {
        redirect_with(res, redirect_uri,
                      "error=invalid_request&error_description=PKCE+S256+required", state);
        return;
    }

    const std::string granted = grant_scope(scope, cfg_.oauth.scopes_supported);

    // Stash the pending request and hand the browser a one-time handle.
    const std::string handle = random_token(18);
    {
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        w.StartObject();
        w.Key("client_id");      w.String(client_id.c_str(), static_cast<rapidjson::SizeType>(client_id.size()));
        w.Key("redirect_uri");   w.String(redirect_uri.c_str(), static_cast<rapidjson::SizeType>(redirect_uri.size()));
        w.Key("code_challenge"); w.String(code_challenge.c_str(), static_cast<rapidjson::SizeType>(code_challenge.size()));
        w.Key("scope");          w.String(granted.c_str(), static_cast<rapidjson::SizeType>(granted.size()));
        w.Key("state");          w.String(state.c_str(), static_cast<rapidjson::SizeType>(state.size()));
        w.Key("resource");       w.String(resource.c_str(), static_cast<rapidjson::SizeType>(resource.size()));
        w.Key("client_name");    w.String(json_str(client, "client_name").c_str());
        w.EndObject();
        cache_.set(kAuthzPrefix + handle, std::string(buf.GetString(), buf.GetSize()),
                   std::chrono::seconds(cfg_.oauth.authz_ttl));
    }

    // Redirect to the web client's consent route, reusing its existing login.
    std::string consent = base_url(req) + uri_prefix_ + cfg_.oauth.consent_path;
    consent += (consent.find('?') == std::string::npos) ? '?' : '&';
    consent += "oauth_consent=" + url_encode(handle);
    res.status(302);
    res.header("Location", consent);
    res.header("Cache-Control", "no-store");
    res.text("");
}

//------------------------------------------------------------------------------
// Consent-card metadata for the web UI. Uses the project {code,msg,data}
// envelope since the SPA's net.get consumes it. No auth: it reveals only the
// public client name + requested scopes for a valid pending handle.

void OAuthService::authorize_info(const server::Request& req, server::Response& res) {
    const std::map<std::string, std::string> q = server::Request::parse_form(req.query);
    const std::string handle = form_get(q, "req");

    std::optional<std::string> authz = handle.empty()
        ? std::nullopt : cache_.get(kAuthzPrefix + handle);
    if (!authz.has_value()) {
        res.error(-1, "This authorization request has expired. Please start over.");
        return;
    }
    rapidjson::Document a;
    a.Parse(authz->c_str(), authz->size());

    const std::string client_name = json_str(a, "client_name");
    const std::string scope       = json_str(a, "scope");
    const std::string redirect    = json_str(a, "redirect_uri");

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("client_name"); w.String(client_name.c_str(),
                                   static_cast<rapidjson::SizeType>(client_name.size()));
    w.Key("redirect_uri"); w.String(redirect.c_str(),
                                    static_cast<rapidjson::SizeType>(redirect.size()));
    w.Key("scopes");
    w.StartArray();
    { const std::vector<std::string> sc = split_scope(scope);
      for (std::size_t i = 0; i < sc.size(); ++i) w.String(sc[i].c_str(),
                                                  static_cast<rapidjson::SizeType>(sc[i].size())); }
    w.EndArray();
    w.EndObject();

    res.ok(std::string(buf.GetString(), buf.GetSize()));
}

//------------------------------------------------------------------------------
// Consent decision. The user is authenticated by their existing bearer token
// (minted by the web login); approving mints the single-use authorization code.

void OAuthService::authorize_decision(const server::Request& req, server::Response& res) {
    jwt::Jwt::VerifyResult vr = jwt_.verify(req.authorization);
    if (!vr.ok() || vr.user_id <= 0) {
        res.error(-1, vr.error.empty() ? "Sign-in required." : vr.error);
        return;
    }

    rapidjson::Document doc;
    if (doc.Parse(req.body.c_str(), req.body.size()).HasParseError() || !doc.IsObject()) {
        res.error(-2, "Invalid JSON body.");
        return;
    }
    const std::string handle = json_str(doc, "req");
    bool approve = false;
    { rapidjson::Value::ConstMemberIterator it = doc.FindMember("approve");
      if (it != doc.MemberEnd() && it->value.IsBool()) approve = it->value.GetBool(); }

    std::optional<std::string> authz = handle.empty()
        ? std::nullopt : cache_.get(kAuthzPrefix + handle);
    if (!authz.has_value()) {
        res.error(-3, "This authorization request has expired. Please start over.");
        return;
    }
    rapidjson::Document a;
    a.Parse(authz->c_str(), authz->size());
    const std::string redirect_uri  = json_str(a, "redirect_uri");
    const std::string state         = json_str(a, "state");
    const std::string client_id     = json_str(a, "client_id");
    const std::string code_challenge = json_str(a, "code_challenge");
    const std::string scope         = json_str(a, "scope");
    const std::string resource      = json_str(a, "resource");

    // The pending request is consumed regardless of the outcome.
    cache_.del(kAuthzPrefix + handle);

    auto reply_redirect = [&](const std::string& url) {
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);

        w.StartObject();
        w.Key("redirect"); w.String(url.c_str(), static_cast<rapidjson::SizeType>(url.size()));
        w.EndObject();

        res.ok(std::string(buf.GetString(), buf.GetSize()));
    };

    auto append = [&](const std::string& base, const std::string& kv) {
        std::string url = base;
        url += (url.find('?') == std::string::npos) ? '?' : '&';
        url += kv;
        if (!state.empty()) { url += "&state="; url += url_encode(state); }
        return url;
    };

    if (!approve) {
        reply_redirect(append(redirect_uri, "error=access_denied"));
        return;
    }

    // Mint the single-use code bound to this user + PKCE challenge.
    const std::string code = random_token(24);
    {
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        w.StartObject();
        w.Key("user_id");        w.Int64(vr.user_id);
        w.Key("email");          { const std::string e = [&]{ rapidjson::Document c;
            if (!c.Parse(vr.claims_json.c_str()).HasParseError()) return json_str(c, "email");
            return std::string(); }();
            w.String(e.c_str(), static_cast<rapidjson::SizeType>(e.size())); }
        w.Key("client_id");      w.String(client_id.c_str(), static_cast<rapidjson::SizeType>(client_id.size()));
        w.Key("redirect_uri");   w.String(redirect_uri.c_str(), static_cast<rapidjson::SizeType>(redirect_uri.size()));
        w.Key("code_challenge"); w.String(code_challenge.c_str(), static_cast<rapidjson::SizeType>(code_challenge.size()));
        w.Key("scope");          w.String(scope.c_str(), static_cast<rapidjson::SizeType>(scope.size()));
        w.Key("resource");       w.String(resource.c_str(), static_cast<rapidjson::SizeType>(resource.size()));
        w.EndObject();
        cache_.set(kCodePrefix + code, std::string(buf.GetString(), buf.GetSize()),
                   std::chrono::seconds(cfg_.oauth.code_ttl));
    }

    reply_redirect(append(redirect_uri, "code=" + url_encode(code)));
}

//------------------------------------------------------------------------------
// Token endpoint (RFC 6749 sec.4.1.3 + 6)

void OAuthService::token(const server::Request& req, server::Response& res) {
    res.header("Cache-Control", "no-store");
    res.header("Pragma", "no-cache");

    const std::map<std::string, std::string> p = server::Request::parse_form(req.body);
    const std::string grant_type = form_get(p, "grant_type");

    // Issue an access + refresh token pair for `user_id`/`scope`, RFC 6749 sec.5.1.
    auto issue_tokens = [&](std::int64_t user_id, const std::string& email,
                            const std::string& scope) {
        std::unordered_map<std::string, std::string> access_claims;
        if (!email.empty()) access_claims["email"] = email;
        access_claims["token_type"] = "oauth_access_token";
        if (!scope.empty()) access_claims["scope"] = scope;
        const std::string access = jwt_.generate(user_id, access_claims, 0);

        std::unordered_map<std::string, std::string> refresh_claims;
        refresh_claims["token_type"] = "oauth_refresh_token";
        if (!scope.empty()) refresh_claims["scope"] = scope;
        const std::string refresh = jwt_.generate(user_id, refresh_claims, jwt_.expires_in() * 2);

        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        w.StartObject();
        w.Key("access_token");  w.String(access.c_str(), static_cast<rapidjson::SizeType>(access.size()));
        w.Key("token_type");    w.String("Bearer");
        w.Key("expires_in");    w.Int64(jwt_.expires_in());
        w.Key("refresh_token"); w.String(refresh.c_str(), static_cast<rapidjson::SizeType>(refresh.size()));
        if (!scope.empty()) { w.Key("scope"); w.String(scope.c_str(),
                                              static_cast<rapidjson::SizeType>(scope.size())); }
        w.EndObject();
        send_json(res, 200, std::string(buf.GetString(), buf.GetSize()));
    };

    if (grant_type == "authorization_code") {
        const std::string code          = form_get(p, "code");
        const std::string code_verifier = form_get(p, "code_verifier");
        const std::string redirect_uri  = form_get(p, "redirect_uri");
        const std::string client_id     = form_get(p, "client_id");
        if (code.empty() || code_verifier.empty()) {
            send_oauth_error(res, 400, "invalid_request", "code and code_verifier are required");
            return;
        }
        std::optional<std::string> rec = cache_.get(kCodePrefix + code);
        // Single-use: consume the code immediately, before any validation, so a
        // replay (even a concurrent one) cannot find it again.
        cache_.del(kCodePrefix + code);
        if (!rec.has_value()) {
            send_oauth_error(res, 400, "invalid_grant", "Authorization code is invalid or expired");
            return;
        }
        rapidjson::Document g;
        g.Parse(rec->c_str(), rec->size());
        const std::int64_t user_id = (g.IsObject() && g.HasMember("user_id") && g["user_id"].IsInt64())
                                     ? g["user_id"].GetInt64() : 0;
        if (user_id <= 0) {
            send_oauth_error(res, 400, "invalid_grant", "Authorization code is invalid");
            return;
        }
        if (!client_id.empty() && client_id != json_str(g, "client_id")) {
            send_oauth_error(res, 400, "invalid_grant", "client_id mismatch");
            return;
        }
        if (redirect_uri != json_str(g, "redirect_uri")) {
            send_oauth_error(res, 400, "invalid_grant", "redirect_uri mismatch");
            return;
        }
        if (!verify_pkce_s256(code_verifier, json_str(g, "code_challenge"))) {
            send_oauth_error(res, 400, "invalid_grant", "PKCE verification failed");
            return;
        }
        issue_tokens(user_id, json_str(g, "email"), json_str(g, "scope"));
        return;
    }

    if (grant_type == "refresh_token") {
        const std::string refresh = form_get(p, "refresh_token");
        if (refresh.empty()) {
            send_oauth_error(res, 400, "invalid_request", "refresh_token is required");
            return;
        }
        jwt::Jwt::VerifyResult vr = jwt_.verify(refresh);
        if (!vr.ok() || vr.user_id <= 0) {
            send_oauth_error(res, 400, "invalid_grant", "Refresh token is invalid or expired");
            return;
        }
        // Only a token explicitly minted as a refresh token may refresh.
        std::string token_type, scope, email;
        { rapidjson::Document c;
          if (!c.Parse(vr.claims_json.c_str()).HasParseError()) {
              token_type = json_str(c, "token_type");
              scope = json_str(c, "scope");
              email = json_str(c, "email");
          } }
        if (token_type != "oauth_refresh_token") {
            send_oauth_error(res, 400, "invalid_grant", "Not a refresh token");
            return;
        }
        // A narrower scope may be requested on refresh (RFC 6749 sec.6); never wider.
        const std::string requested = form_get(p, "scope");
        const std::string out_scope = requested.empty() ? scope : grant_scope(requested, scope);
        issue_tokens(vr.user_id, email, out_scope);
        return;
    }

    send_oauth_error(res, 400, "unsupported_grant_type", "Unsupported grant_type: " + grant_type);
}

//------------------------------------------------------------------------------
// Revocation (RFC 7009). Best-effort: authorization codes live in the cache and
// can be dropped; refresh/access tokens are stateless JWTs, so there is nothing
// server-side to delete for them. Per the RFC we return 200 regardless.

void OAuthService::revoke(const server::Request& req, server::Response& res) {
    const std::map<std::string, std::string> p = server::Request::parse_form(req.body);
    const std::string token = form_get(p, "token");
    if (!token.empty()) {
        cache_.del(kCodePrefix + token);
    }
    res.status(200);
    res.json("{}");
}

}}
