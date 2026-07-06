#include "health/vendor_link.hpp"

#include "config/fernet.hpp"
#include "database/database.hpp"
#include "platform/clock.hpp"   // now_unix_ms
#include "health/vendor/registry.hpp"

#include <cstdint>
#include <ctime>
#include <exception>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace mirobody { namespace health {
namespace {

// Format Unix epoch ms as an ISO-8601 UTC instant, for the verify_consent probe's
// fetch bounds. (The per-vendor clients trim this to the granularity they need.)
std::string iso_utc(std::int64_t unix_ms) {
    std::time_t t = static_cast<std::time_t>(unix_ms / 1000);
    std::tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

// Fernet ciphers for vendor-token encryption at rest, newest last (last encrypts,
// all decrypt). Empty when VENDOR_TOKEN_ENCRYPTION_KEY is unset — the signal that
// DB token storage is disabled. Bad keys are skipped.
std::vector<std::shared_ptr<encrypt::Fernet> > token_ciphers(const Config& cfg) {
    std::vector<std::shared_ptr<encrypt::Fernet> > out;
    for (std::size_t i = 0; i < cfg.vendor_token_encryption_keys.size(); ++i) {
        const std::string& k = cfg.vendor_token_encryption_keys[i];
        if (k.empty()) continue;
        try {
            out.push_back(std::make_shared<encrypt::Fernet>(k));
        } catch (const encrypt::FernetError&) {
            // A malformed key is skipped rather than aborting; a fully empty result
            // simply disables persistence.
        }
    }
    return out;
}

// Encrypt `plain` with the newest key. Returns false (leaving *out untouched) when
// no key is configured — callers then skip persisting rather than store plaintext.
bool token_encrypt(const Config& cfg, const std::string& plain, std::string* out) {
    std::vector<std::shared_ptr<encrypt::Fernet> > c = token_ciphers(cfg);
    if (c.empty()) return false;
    *out = c.back()->encrypt(plain);
    return true;
}

// Decrypt a stored ciphertext, trying each key. "" for empty input or on failure
// (e.g. after the key it was encrypted under was rotated out).
std::string token_decrypt(const Config& cfg, const std::string& cipher) {
    if (cipher.empty()) return std::string();
    std::vector<std::shared_ptr<encrypt::Fernet> > cs = token_ciphers(cfg);
    for (std::size_t i = 0; i < cs.size(); ++i) {
        try {
            return cs[i]->decrypt(cipher);
        } catch (const encrypt::FernetError&) {
        }
    }
    return std::string();
}

// Does a vendor error look like an auth rejection worth a refresh + retry? The
// per-vendor clients embed the HTTP status in the message ("... (HTTP 401): ...").
bool is_auth_error(const std::string& msg) {
    return msg.find("401") != std::string::npos || msg.find("403") != std::string::npos;
}

// True when `cipher` decrypts under the PRIMARY (last/newest) key alone — i.e. it is
// already at the current key and needs no re-encryption. Empty / no keys => true (no
// migration to do). Fernet doesn't tag which key a token used, so this trial-decrypt
// with just the newest key is how we tell "already primary" from "under an old key".
bool under_primary_key(const Config& cfg, const std::string& cipher) {
    if (cipher.empty()) return true;
    std::vector<std::shared_ptr<encrypt::Fernet> > cs = token_ciphers(cfg);
    if (cs.empty()) return true;
    try {
        cs.back()->decrypt(cipher);
        return true;
    } catch (const encrypt::FernetError&) {
        return false;
    }
}

// Re-encrypt `cipher` under the primary key. Returns "" when there is nothing to do
// (empty, already primary, or undecryptable under any current key — a dead token left
// for the user to re-connect); otherwise the new primary-key ciphertext.
std::string reencrypt_to_primary(const Config& cfg, const std::string& cipher) {
    if (cipher.empty() || under_primary_key(cfg, cipher)) return std::string();
    const std::string plain = token_decrypt(cfg, cipher);
    if (plain.empty()) return std::string();
    std::string out;
    return token_encrypt(cfg, plain, &out) ? out : std::string();
}

}  // namespace

vendor::VendorConfig vendor_config(const Config& cfg, const std::string& vendor_id) {
    // Base: the MIROBODY_VENDOR_<ID>_* env convention. Still the path for the vendor
    // CLI and for aggregators not (yet) wired to central config.
    vendor::VendorConfig vc = vendor::VendorConfig::from_env(vendor_id);

    // Overlay the central config credentials — the clean <ID>_CLIENT_ID / _SECRET /
    // _API_KEY / _BASE_URL keys, read from YAML or env through the config store.
    // When set they win over the env fallback above.
    std::unordered_map<std::string, VendorCredentials>::const_iterator it =
        cfg.vendor_credentials.find(vendor_id);
    if (it != cfg.vendor_credentials.end()) {
        const VendorCredentials& c = it->second;
        if (!c.client_id.empty())     vc.client_id     = c.client_id;
        if (!c.client_secret.empty()) vc.client_secret = c.client_secret;
        if (!c.api_key.empty())       vc.api_key       = c.api_key;
        if (!c.base_url.empty())      vc.base_url      = c.base_url;
    }

    if (vendor_id == "vitalera") {
        // Central config carries Vitalera's pre-issued bearer.
        if (!cfg.vitalera.api_key.empty()) {
            vc.api_key = cfg.vitalera.api_key;
        }
    } else if (vendor_id == "dexcom") {
        // Map the Dexcom deployment name to the API host, unless a base_url is
        // already set (DEXCOM_BASE_URL / env). Empty/"sandbox" leaves base_url empty
        // so the client falls back to its sandbox default.
        if (vc.base_url.empty()) {
            const std::string& e = cfg.dexcom.environment;
            if (e == "us" || e == "production") {
                vc.base_url = "https://api.dexcom.com";
            } else if (e == "eu" || e == "ous") {
                vc.base_url = "https://api.dexcom.eu";
            }
        }
    }
    return vc;
}

//------------------------------------------------------------------------------

bool VendorLinkStore::get(std::int64_t user_id, const std::string& vendor_id, Link* out) {
    database::Result r = db_.execute(
        "SELECT external_user_id, verified_at, access_token, refresh_token, token_expires_at "
        "FROM user_vendor_accounts WHERE user_id=? AND vendor_id=?;",
        {user_id, vendor_id});
    if (r.rows.empty() || r.rows[0].empty()) return false;
    if (out) {
        out->external_user_id = r.rows[0][0].as_text();
        out->verified = !r.rows[0][1].is_null();
        out->access_token  = r.rows[0][2].is_null() ? std::string() : r.rows[0][2].as_text();
        out->refresh_token = r.rows[0][3].is_null() ? std::string() : r.rows[0][3].as_text();
        out->token_expires_at = r.rows[0][4].is_null() ? 0 : r.rows[0][4].as_int();
    }
    return true;
}

void VendorLinkStore::save_tokens(std::int64_t user_id, const std::string& vendor_id,
                                  const std::string& access_enc, const std::string& refresh_enc,
                                  std::int64_t expires_at) {
    db_.execute(
        "UPDATE user_vendor_accounts SET access_token=?, refresh_token=?, token_expires_at=?, updated_at=? "
        "WHERE user_id=? AND vendor_id=?;",
        {access_enc, refresh_enc, expires_at, platform::now_unix_ms(), user_id, vendor_id});
}

void VendorLinkStore::clear_tokens(std::int64_t user_id, const std::string& vendor_id) {
    db_.execute(
        "UPDATE user_vendor_accounts SET access_token=NULL, refresh_token=NULL, token_expires_at=NULL, "
        "updated_at=? WHERE user_id=? AND vendor_id=?;",
        {platform::now_unix_ms(), user_id, vendor_id});
}

void VendorLinkStore::remove(std::int64_t user_id, const std::string& vendor_id) {
    db_.execute("DELETE FROM user_vendor_accounts WHERE user_id=? AND vendor_id=?;",
                {user_id, vendor_id});
}

std::vector<VendorLinkStore::LinkSummary> VendorLinkStore::list(std::int64_t user_id) {
    database::Result r = db_.execute(
        "SELECT vendor_id, verified_at, access_token, updated_at FROM user_vendor_accounts "
        "WHERE user_id=? ORDER BY vendor_id;",
        {user_id});
    std::vector<LinkSummary> out;
    for (std::size_t i = 0; i < r.rows.size(); ++i) {
        if (r.rows[i].size() < 4) continue;
        LinkSummary s;
        s.vendor_id  = r.rows[i][0].as_text();
        s.verified   = !r.rows[i][1].is_null();
        s.has_token  = !r.rows[i][2].is_null();
        s.updated_at = r.rows[i][3].is_null() ? 0 : r.rows[i][3].as_int();
        out.push_back(s);
    }
    return out;
}

std::vector<VendorLinkStore::TokenRow> VendorLinkStore::all_token_rows() {
    database::Result r = db_.execute(
        "SELECT user_id, vendor_id, access_token, refresh_token, token_expires_at "
        "FROM user_vendor_accounts WHERE access_token IS NOT NULL OR refresh_token IS NOT NULL;",
        {});
    std::vector<TokenRow> out;
    for (std::size_t i = 0; i < r.rows.size(); ++i) {
        if (r.rows[i].size() < 5) continue;
        TokenRow t;
        t.user_id          = r.rows[i][0].as_int();
        t.vendor_id        = r.rows[i][1].as_text();
        t.access_token     = r.rows[i][2].is_null() ? std::string() : r.rows[i][2].as_text();
        t.refresh_token    = r.rows[i][3].is_null() ? std::string() : r.rows[i][3].as_text();
        t.token_expires_at = r.rows[i][4].is_null() ? 0 : r.rows[i][4].as_int();
        out.push_back(t);
    }
    return out;
}

void VendorLinkStore::upsert_pending(std::int64_t user_id, const std::string& vendor_id,
                                     const std::string& external_user_id) {
    // SELECT-then-UPDATE/INSERT keeps the SQL portable across backends (no
    // ON CONFLICT / ON DUPLICATE KEY dialect split). Re-binding a link updates
    // the external id and resets it to pending (verified_at NULL).
    const std::int64_t now = platform::now_unix_ms();
    if (get(user_id, vendor_id, nullptr)) {
        // Re-binding replaces the id, clears verification, and drops any stale tokens.
        db_.execute(
            "UPDATE user_vendor_accounts SET external_user_id=?, updated_at=?, verified_at=NULL, "
            "access_token=NULL, refresh_token=NULL, token_expires_at=NULL "
            "WHERE user_id=? AND vendor_id=?;",
            {external_user_id, now, user_id, vendor_id});
        return;
    }
    db_.execute(
        "INSERT INTO user_vendor_accounts "
        "(user_id, vendor_id, external_user_id, created_at, updated_at) VALUES (?, ?, ?, ?, ?);",
        {user_id, vendor_id, external_user_id, now, now});
}

bool VendorLinkStore::mark_verified(std::int64_t user_id, const std::string& vendor_id) {
    const std::int64_t now = platform::now_unix_ms();
    database::Result r = db_.execute(
        "UPDATE user_vendor_accounts SET verified_at=?, updated_at=? WHERE user_id=? AND vendor_id=?;",
        {now, now, user_id, vendor_id});
    return r.rows_affected > 0;
}

//------------------------------------------------------------------------------

std::string fetch_for_user(VendorLinkStore& store, const Config& cfg,
                           std::int64_t user_id, const std::string& vendor_id,
                           vendor::DataDomain domain,
                           const std::string& start_iso, const std::string& end_iso,
                           std::string* err) {
    VendorLinkStore::Link link;
    bool found = false;
    try {
        found = store.get(user_id, vendor_id, &link);
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return std::string();
    }
    if (!found) {
        if (err) *err = vendor_id + ": no linked account for this user (bind one first)";
        return std::string();
    }
    if (!link.verified) {
        if (err) *err = vendor_id + ": account link is not verified yet "
                        "(no data access until ownership is confirmed)";
        return std::string();
    }

    try {
        vendor::VendorConfig vc = vendor_config(cfg, vendor_id);

        // The user's own stored token (decrypted) overrides the configured credential,
        // so each user pulls their own data.
        std::string access = token_decrypt(cfg, link.access_token);
        const std::string refresh = token_decrypt(cfg, link.refresh_token);
        bool refreshed = false;

        // Refresh the access token via the vendor, persist the rotated tokens, and
        // update `access` / `refreshed`. False when there's no refresh token or the
        // refresh call itself fails.
        auto do_refresh = [&]() -> bool {
            if (refresh.empty()) return false;
            try {
                std::unique_ptr<vendor::Vendor> rv = vendor::open_vendor(vendor_id, vc);
                vendor::TokenSet t = rv->refresh(refresh);
                std::string ae, re;
                if (token_encrypt(cfg, t.access_token, &ae)) {
                    // Keep the prior refresh token when the vendor didn't rotate one.
                    if (!t.refresh_token.empty()) token_encrypt(cfg, t.refresh_token, &re);
                    else                          re = link.refresh_token;
                    const std::int64_t exp = t.expires_in > 0
                        ? platform::now_unix_ms() + t.expires_in * 1000 : 0;
                    store.save_tokens(user_id, vendor_id, ae, re, exp);
                }
                access = t.access_token;
                refreshed = true;
                return true;
            } catch (const std::exception&) {
                return false;
            }
        };

        // Proactive: refresh when the stored access token has expired (or is within 60s).
        if (!access.empty() && link.token_expires_at > 0 &&
            platform::now_unix_ms() >= link.token_expires_at - 60000) {
            do_refresh();
        }
        if (!access.empty()) vc.api_key = access;

        std::unique_ptr<vendor::Vendor> v = vendor::open_vendor(vendor_id, vc);
        try {
            return v->fetch(link.external_user_id, domain, start_iso, end_iso);
        } catch (const std::exception& fe) {
            // Reactive: the vendor rejected the token (401/403) though our clock said
            // it was still valid (early revocation / early expiry). Refresh once and
            // retry — but only if we didn't already refresh proactively above.
            if (!refreshed && is_auth_error(fe.what()) && do_refresh()) {
                vc.api_key = access;
                std::unique_ptr<vendor::Vendor> v2 = vendor::open_vendor(vendor_id, vc);
                return v2->fetch(link.external_user_id, domain, start_iso, end_iso);
            }
            throw;
        }
    } catch (const std::exception& e) {
        // vendor::VendorError derives from std::runtime_error; this also catches
        // an unknown-vendor error from open_vendor.
        if (err) *err = e.what();
        return std::string();
    }
}

//------------------------------------------------------------------------------

bool verify_consent(VendorLinkStore& store, const Config& cfg, std::int64_t user_id,
                    const std::string& vendor_id, const std::string& external_user_id,
                    const std::string& code, const std::string& redirect_uri,
                    std::string* err) {
    vendor::VendorConfig vc = vendor_config(cfg, vendor_id);

    std::unique_ptr<vendor::Vendor> v;
    try {
        v = vendor::open_vendor(vendor_id, vc);
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }

    // Mode 1: OAuth authorization code — exchange it (proves ownership), then persist
    // the resulting tokens encrypted (when a key is configured).
    if (!code.empty()) {
        vendor::TokenSet tok;
        try {
            tok = v->exchange_code(code, redirect_uri);
        } catch (const std::exception& e) {
            if (err) *err = vendor_id + ": OAuth code exchange failed: " + e.what();
            return false;
        }
        std::string access_enc;
        if (token_encrypt(cfg, tok.access_token, &access_enc)) {
            std::string refresh_enc;
            if (!tok.refresh_token.empty()) token_encrypt(cfg, tok.refresh_token, &refresh_enc);
            const std::int64_t exp = tok.expires_in > 0
                ? platform::now_unix_ms() + tok.expires_in * 1000 : 0;
            try {
                store.save_tokens(user_id, vendor_id, access_enc, refresh_enc, exp);
            } catch (const std::exception& e) {
                if (err) *err = vendor_id + ": token persist failed: " + e.what();
                return false;
            }
        }
        // else: no VENDOR_TOKEN_ENCRYPTION_KEY — ownership is proven but tokens are
        // not persisted (never stored in the clear); fetch falls back to config creds.
        return true;
    }

    // Mode 2: no code — probe the configured credential. A successful read (any
    // advertised domain returns without throwing) proves it works for
    // external_user_id. Vendors with no synchronous pull (garmin push, polar
    // training) fail the probe on those domains and fall through.
    if (!vc.configured()) {
        if (err) {
            *err = vendor_id + ": no OAuth code and no configured credential to verify with "
                   "(post {code, redirect_uri}, or set the vendor's <ID>_* keys).";
        }
        return false;
    }
    const vendor::VendorInfo& info = v->info();
    if (info.domains.empty()) {
        if (err) *err = vendor_id + ": vendor brokers no data to verify against.";
        return false;
    }
    const std::int64_t now = platform::now_unix_ms();
    const std::string end   = iso_utc(now);
    const std::string start = iso_utc(now - 2LL * 24 * 3600 * 1000);   // last 2 days
    std::string last_err;
    for (std::size_t i = 0; i < info.domains.size(); ++i) {
        try {
            v->fetch(external_user_id, info.domains[i], start, end);
            return true;
        } catch (const std::exception& e) {
            last_err = e.what();
        }
    }
    if (err) {
        *err = vendor_id + ": ownership verification failed (no domain readable): " + last_err;
    }
    return false;
}

void reencrypt_vendor_tokens(VendorLinkStore& store, const Config& cfg) {
    if (cfg.vendor_token_encryption_keys.size() < 2) return;   // nothing to migrate
    std::vector<VendorLinkStore::TokenRow> rows = store.all_token_rows();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const std::string na = reencrypt_to_primary(cfg, rows[i].access_token);
        const std::string nr = reencrypt_to_primary(cfg, rows[i].refresh_token);
        if (!na.empty() || !nr.empty()) {
            store.save_tokens(rows[i].user_id, rows[i].vendor_id,
                              na.empty() ? rows[i].access_token : na,
                              nr.empty() ? rows[i].refresh_token : nr,
                              rows[i].token_expires_at);
        }
    }
}

bool oauth_connect(VendorLinkStore& store, const Config& cfg, std::int64_t user_id,
                   const std::string& vendor_id, const std::string& code,
                   const std::string& redirect_uri, std::string* err) {
    try {
        vendor::VendorConfig vc = vendor_config(cfg, vendor_id);
        std::unique_ptr<vendor::Vendor> v = vendor::open_vendor(vendor_id, vc);
        vendor::TokenSet tok = v->exchange_code(code, redirect_uri);   // throws -> err

        // OAuth has no external id up front — use a per-user synthetic id, unique
        // under UNIQUE(vendor_id, external_user_id). upsert_pending ensures the row.
        const std::string ext = "oauth-user-" + std::to_string(user_id);
        store.upsert_pending(user_id, vendor_id, ext);

        // Persist tokens (encrypted) when a key is configured, then mark verified.
        std::string access_enc;
        if (token_encrypt(cfg, tok.access_token, &access_enc)) {
            std::string refresh_enc;
            if (!tok.refresh_token.empty()) token_encrypt(cfg, tok.refresh_token, &refresh_enc);
            const std::int64_t exp = tok.expires_in > 0
                ? platform::now_unix_ms() + tok.expires_in * 1000 : 0;
            store.save_tokens(user_id, vendor_id, access_enc, refresh_enc, exp);
        }
        store.mark_verified(user_id, vendor_id);
        return true;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}

bool unlink_for_user(VendorLinkStore& store, const Config& cfg,
                     std::int64_t user_id, const std::string& vendor_id) {
    VendorLinkStore::Link link;
    const bool found = store.get(user_id, vendor_id, &link);   // may throw (DB) -> caller catches
    if (found) {
        // Best-effort: revoke the grant at the vendor with the user's own token (or
        // the configured credential). A vendor with no revoke endpoint (stub throws)
        // or a transport error must not block the local deletion below.
        try {
            vendor::VendorConfig vc = vendor_config(cfg, vendor_id);
            const std::string access = token_decrypt(cfg, link.access_token);
            if (!access.empty()) vc.api_key = access;
            if (vc.configured()) {
                std::unique_ptr<vendor::Vendor> v = vendor::open_vendor(vendor_id, vc);
                v->revoke(link.external_user_id, std::string());
            }
        } catch (const std::exception&) {
        }
    }
    store.remove(user_id, vendor_id);   // may throw (DB) -> caller catches
    return found;
}

}}  // namespace mirobody::health
