#pragma once

// User <-> health-vendor account links and the verified-gated fetch path.
//
// A mirobody user binds their id on an external health-data vendor (a Vitalera
// patient id, a Terra user UUID, ...) the same way they bind an email: submit
// it, prove ownership, then it is usable. The link lives in user_vendor_accounts
// (res/sql/<dialect>/1_health.sql); only a VERIFIED link may be used to pull
// data, so a self-asserted id can never read another person's records.
//
// This module is the seam between the central Config, the user_vendor_accounts
// table, and the vendor clients (src/health/vendor/). The HTTP surface is
// health::VendorService (vendor_service.hpp).

#include "config/config.hpp"
#include "health/vendor/vendor.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mirobody { namespace database { class Database; } }

namespace mirobody { namespace health {

// Build the VendorConfig for `vendor_id` from process config + environment.
// Starts from the per-vendor MIROBODY_VENDOR_<ID>_* env convention
// (VendorConfig::from_env) and overlays the central Config where it carries the
// vendor's credentials -- today that is Vitalera's VITALERA_API_KEY (used as the
// pre-issued bearer; the client_id/secret mint flow still reads the env vars).
vendor::VendorConfig vendor_config(const Config& cfg, const std::string& vendor_id);

//------------------------------------------------------------------------------

// CRUD over user_vendor_accounts. Borrows the Database (not owned); one per use
// site, like fhir::FhirStore. Not thread-safe (the Database is not).
class VendorLinkStore {
public:
    explicit VendorLinkStore(database::Database& db) : db_(db) {}

    struct Link {
        std::string  external_user_id;
        bool         verified = false;      // verified_at IS NOT NULL
        std::string  access_token;          // encrypted ciphertext as stored ("" = none)
        std::string  refresh_token;         // encrypted ciphertext as stored ("" = none)
        std::int64_t token_expires_at = 0;  // access token expiry, unix ms; 0 = unknown/non-expiring
    };

    // Read the link for (user_id, vendor_id). Returns false when none exists.
    // Throws std::exception on a database error.
    bool get(std::int64_t user_id, const std::string& vendor_id, Link* out);

    // A user's connected vendors, for listing (no secrets — `has_token` just says
    // whether a stored OAuth token is present).
    struct LinkSummary {
        std::string  vendor_id;
        bool         verified = false;
        bool         has_token = false;
        std::int64_t updated_at = 0;   // last change, unix ms (0 = never stamped)
    };

    // All of `user_id`'s vendor links, ordered by vendor_id. Throws on a DB error.
    std::vector<LinkSummary> list(std::int64_t user_id);

    // One token-bearing row (across all users), for the key-rotation re-encrypt pass.
    // access_token / refresh_token are the stored ciphertexts.
    struct TokenRow {
        std::int64_t user_id = 0;
        std::string  vendor_id;
        std::string  access_token;
        std::string  refresh_token;
        std::int64_t token_expires_at = 0;
    };

    // Every row that has a stored token (any user). For reencrypt_vendor_tokens.
    std::vector<TokenRow> all_token_rows();

    // Persist the link's OAuth tokens. `access_enc` / `refresh_enc` are the Fernet
    // CIPHERTEXTS (already encrypted; `refresh_enc` "" when the vendor issues none);
    // `expires_at` is the access token expiry in unix ms (0 = unknown/non-expiring).
    // Throws on a database error.
    void save_tokens(std::int64_t user_id, const std::string& vendor_id,
                     const std::string& access_enc, const std::string& refresh_enc,
                     std::int64_t expires_at);

    // Clear stored tokens (on revoke / disconnect). Safe when none are stored.
    void clear_tokens(std::int64_t user_id, const std::string& vendor_id);

    // Delete the (user_id, vendor_id) link row entirely — the full disconnect
    // (tokens included). A no-op when no such row exists. Throws on a DB error.
    void remove(std::int64_t user_id, const std::string& vendor_id);

    // Create or replace the (user_id, vendor_id) link as PENDING (verified_at
    // NULL), recording the vendor's external id. Re-binding updates the id and
    // clears any prior verification. Throws on a database error -- including the
    // UNIQUE(vendor_id, external_user_id) ownership backstop when another user
    // already holds that vendor account.
    void upsert_pending(std::int64_t user_id, const std::string& vendor_id,
                        const std::string& external_user_id);

    // Mark the (user_id, vendor_id) link verified (stamps verified_at = now).
    // Returns false when no such row exists.
    bool mark_verified(std::int64_t user_id, const std::string& vendor_id);

private:
    database::Database& db_;
};

//------------------------------------------------------------------------------

// Fetch `domain` data for `user_id` from `vendor_id` over [start_iso, end_iso].
// Resolves the user's VERIFIED external id from the link store, builds the vendor
// client from config, and calls Vendor::fetch. Returns the vendor JSON on
// success; on failure returns "" and writes the reason to *err -- including when
// the user has no link, the link is unverified (no data access until ownership
// is proven), the vendor is unknown, or the vendor call throws.
//
// Uses the user's stored, encrypted OAuth token when present (refreshing it via the
// vendor's refresh grant if expired and re-persisting), else the configured <ID>_*
// credential — so multi-user deployments pull each user's data with their own token.
std::string fetch_for_user(VendorLinkStore& store, const Config& cfg,
                           std::int64_t user_id, const std::string& vendor_id,
                           vendor::DataDomain domain,
                           const std::string& start_iso, const std::string& end_iso,
                           std::string* err);

//------------------------------------------------------------------------------

// THE VERIFICATION SEAM. Prove that `external_user_id` on `vendor_id` belongs to
// the caller and, on success, persist the user's OAuth tokens (encrypted). Returns
// true when ownership is proven, else false with the reason in *err.
//
// Two modes:
//   * `code` non-empty — the OAuth authorization code from the vendor's redirect.
//     The server exchanges it (Vendor::exchange_code with `redirect_uri`); a
//     successful exchange proves ownership. The resulting access/refresh tokens are
//     stored via store.save_tokens, ENCRYPTED, when VENDOR_TOKEN_ENCRYPTION_KEY is
//     set (with no key, ownership is still proven but nothing is persisted — never
//     stored in the clear).
//   * `code` empty — fall back to a credential PROBE: read a small recent window
//     with the configured <ID>_* credential; any successful domain read passes.
//     (Garmin push / Polar training have no synchronous pull and fail the probe.)
bool verify_consent(VendorLinkStore& store, const Config& cfg, std::int64_t user_id,
                    const std::string& vendor_id, const std::string& external_user_id,
                    const std::string& code, const std::string& redirect_uri,
                    std::string* err);

// Complete a web OAuth connect: exchange the authorization `code` at the vendor
// (Vendor::exchange_code with `redirect_uri`), create/reset the link row, persist the
// resulting tokens (encrypted, when VENDOR_TOKEN_ENCRYPTION_KEY is set), and mark the
// link verified. Returns true on success; false with the reason in *err. Used by the
// GET /vendors/callback redirect handler.
bool oauth_connect(VendorLinkStore& store, const Config& cfg, std::int64_t user_id,
                   const std::string& vendor_id, const std::string& code,
                   const std::string& redirect_uri, std::string* err);

// Re-encrypt any stored vendor tokens that sit under a non-primary key to the current
// (last) VENDOR_TOKEN_ENCRYPTION_KEY. No-op unless >=2 keys are configured (a rotation
// window). Run once at startup, before serving, so an old key can be dropped promptly
// after a rotation instead of waiting for lazy per-token re-encryption. Throws on a DB
// error.
void reencrypt_vendor_tokens(VendorLinkStore& store, const Config& cfg);

// Disconnect the user from `vendor_id`: best-effort revoke the grant at the vendor
// (Vendor::revoke, using the user's stored token when present — a vendor without a
// revoke endpoint or a transport error does NOT block local deletion), then delete
// the link row (tokens included). Returns true when a link existed. Throws only on a
// database error from the deletion.
bool unlink_for_user(VendorLinkStore& store, const Config& cfg,
                     std::int64_t user_id, const std::string& vendor_id);

}}  // namespace mirobody::health
