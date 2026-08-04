#pragma once

// Shared OAuth2 token-endpoint helper for the device-brand vendor clients.
//
// Most of the direct OAuth2 vendors (dexcom, oura, whoop, fitbit, polar, …) speak
// the standard RFC 6749 token endpoint: a form-encoded POST with a `grant_type` and
// either client credentials in the body or via HTTP Basic, returning a JSON body
// { access_token, refresh_token?, expires_in? }. This centralizes that request so
// each vendor's exchange_code()/refresh() is just "which URL, which params, which
// auth style". Vendors with a non-standard envelope (e.g. Withings) do their own.

#include "health/vendor/vendor.hpp"

#include <string>
#include <utility>
#include <vector>

namespace mirobody { namespace vendor {

// POST a standard OAuth2 token request (application/x-www-form-urlencoded) of
// `params` to `token_url`. When `basic_user` is non-empty, client authentication is
// HTTP Basic (basic_user:basic_pass); otherwise the caller should include the client
// credentials among `params`. Parses the standard JSON response and returns its
// TokenSet. Throws VendorError on transport error, a non-2xx status, or a response
// missing access_token.
TokenSet oauth2_token_request(const std::string& token_url,
                              const std::vector<std::pair<std::string, std::string> >& params,
                              const std::string& basic_user = std::string(),
                              const std::string& basic_pass = std::string());

}}  // namespace mirobody::vendor
