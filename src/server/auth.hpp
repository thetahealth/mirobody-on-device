#pragma once

#include "jwt/jwt.hpp"
#include "server/router.hpp"

#include <rapidjson/document.h>

#include <string>

namespace mirobody { namespace server {

// Wrap `inner` so it only runs after a valid bearer JWT is presented. On
// failure the returned handler responds 401 with a WWW-Authenticate header and
// a JSON error body; verify() strips a leading "Bearer " from the header value
// itself. `verifier` is borrowed by reference and must outlive the returned
// handler (and therefore the Router it is installed on).
//
// The token's `sub` is an opaque encoding of the user id (encrypt::encode_int64
// keyed by verifier.salt()); require_auth decodes it back and sets req.user_id
// to the raw id. A `sub` that does not decode is rejected as invalid.
inline HttpHandler require_auth(const jwt::Jwt& verifier, HttpHandler inner) {
    return [&verifier, inner](const Request& req, Response& res) {
        // Prefer the Authorization header; fall back to an access_token / token
        // query parameter for clients that can't set headers (notably browser
        // WebSocket handshakes). verify() strips a leading "Bearer " itself, so
        // the raw header value passes straight through. JWTs are URL-safe, so the
        // verbatim (undecoded) query_str value is exactly the token.
        std::string token = req.authorization;
        if (token.empty()) token = req.query_str("access_token", "");
        if (token.empty()) token = req.query_str("token", "");

        jwt::Jwt::VerifyResult vr = verifier.verify(token);
        if (!vr.ok()) {
            // RFC 6750: omit the error param when no credentials were sent;
            // flag invalid_token when a token was present but rejected.
            res.header("WWW-Authenticate",
                       token.empty() ? "Bearer" : "Bearer error=\"invalid_token\"");
            res.error(-1, vr.error, {}, 401);
            return;
        }

        // verify() already decoded the opaque subject back to the row id. A
        // non-positive id means the `sub` didn't decode (forged / wrong-salt /
        // legacy token) — reject.
        if (vr.user_id <= 0) {
            res.header("WWW-Authenticate", "Bearer error=\"invalid_token\"");
            res.error(-2, "invalid subject", {}, 401);
            return;
        }
        req.user_id = vr.user_id;

        // Surface the identity for downstream handlers (req.user_id / req.email)
        // instead of re-parsing the token. The email is best-effort.
        rapidjson::Document claims;
        if (!claims.Parse(vr.claims_json.c_str()).HasParseError() && claims.IsObject()) {
            rapidjson::Value::ConstMemberIterator it = claims.FindMember("email");
            if (it != claims.MemberEnd() && it->value.IsString()) {
                req.email = std::string(it->value.GetString(), it->value.GetStringLength());
            }
        }

        inner(req, res);
    };
}

}}
