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
        std::string external_user_id;
        bool        verified = false;   // verified_at IS NOT NULL
    };

    // Read the link for (user_id, vendor_id). Returns false when none exists.
    // Throws std::exception on a database error.
    bool get(std::int64_t user_id, const std::string& vendor_id, Link* out);

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
std::string fetch_for_user(VendorLinkStore& store, const Config& cfg,
                           std::int64_t user_id, const std::string& vendor_id,
                           vendor::DataDomain domain,
                           const std::string& start_iso, const std::string& end_iso,
                           std::string* err);

//------------------------------------------------------------------------------

// THE VERIFICATION SEAM. Prove that `external_user_id` on `vendor_id` belongs to
// the caller, using a vendor-specific consent/claim mechanism, before the link is
// marked verified. `consent_token` is whatever that flow hands back (an OAuth
// code, a claim id, ...). Returns true when ownership is proven, else false with
// the reason in *err.
//
// STATUS: stub. Vitalera's patient consent/linking lives in its gated "Monitoreds
// (Patients)" API (not public; support@vitalera.io), so this returns false with a
// "not implemented" reason until that contract is wired in. Filling this in is a
// localized change -- the rest of the bind flow is built around this signature.
bool verify_consent(const Config& cfg, const std::string& vendor_id,
                    const std::string& external_user_id,
                    const std::string& consent_token, std::string* err);

}}  // namespace mirobody::health
