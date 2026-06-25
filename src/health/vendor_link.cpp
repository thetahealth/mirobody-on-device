#include "health/vendor_link.hpp"

#include "database/database.hpp"
#include "platform/clock.hpp"   // now_unix_ms
#include "health/vendor/registry.hpp"

#include <exception>
#include <memory>

namespace mirobody { namespace health {

vendor::VendorConfig vendor_config(const Config& cfg, const std::string& vendor_id) {
    vendor::VendorConfig vc = vendor::VendorConfig::from_env(vendor_id);
    if (vendor_id == "vitalera") {
        // Central config carries Vitalera's pre-issued bearer; the
        // client_id/secret mint flow still reads MIROBODY_VENDOR_VITALERA_*.
        if (!cfg.vitalera.api_key.empty()) {
            vc.api_key = cfg.vitalera.api_key;
        }
        // NOTE: cfg.vitalera.environment names the deployment, but the sandbox
        // host is not yet confirmed (see VitaleraConfig); base_url is left to the
        // env / vendor default until that mapping is known.
    }
    return vc;
}

//------------------------------------------------------------------------------

bool VendorLinkStore::get(std::int64_t user_id, const std::string& vendor_id, Link* out) {
    database::Result r = db_.execute(
        "SELECT external_user_id, verified_at FROM user_vendor_accounts "
        "WHERE user_id=? AND vendor_id=?;",
        {user_id, vendor_id});
    if (r.rows.empty() || r.rows[0].empty()) return false;
    if (out) {
        out->external_user_id = r.rows[0][0].as_text();
        out->verified = !r.rows[0][1].is_null();
    }
    return true;
}

void VendorLinkStore::upsert_pending(std::int64_t user_id, const std::string& vendor_id,
                                     const std::string& external_user_id) {
    // SELECT-then-UPDATE/INSERT keeps the SQL portable across backends (no
    // ON CONFLICT / ON DUPLICATE KEY dialect split). Re-binding a link updates
    // the external id and resets it to pending (verified_at NULL).
    if (get(user_id, vendor_id, nullptr)) {
        db_.execute(
            "UPDATE user_vendor_accounts SET external_user_id=?, verified_at=NULL "
            "WHERE user_id=? AND vendor_id=?;",
            {external_user_id, user_id, vendor_id});
        return;
    }
    db_.execute(
        "INSERT INTO user_vendor_accounts "
        "(user_id, vendor_id, external_user_id, created_at) VALUES (?, ?, ?, ?);",
        {user_id, vendor_id, external_user_id, platform::now_unix_ms()});
}

bool VendorLinkStore::mark_verified(std::int64_t user_id, const std::string& vendor_id) {
    database::Result r = db_.execute(
        "UPDATE user_vendor_accounts SET verified_at=? WHERE user_id=? AND vendor_id=?;",
        {platform::now_unix_ms(), user_id, vendor_id});
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
        std::unique_ptr<vendor::Vendor> v = vendor::open_vendor(vendor_id, vc);
        return v->fetch(link.external_user_id, domain, start_iso, end_iso);
    } catch (const std::exception& e) {
        // vendor::VendorError derives from std::runtime_error; this also catches
        // an unknown-vendor error from open_vendor.
        if (err) *err = e.what();
        return std::string();
    }
}

//------------------------------------------------------------------------------

bool verify_consent(const Config& /*cfg*/, const std::string& vendor_id,
                    const std::string& /*external_user_id*/,
                    const std::string& /*consent_token*/, std::string* err) {
    // SEAM: wire the vendor's consent/claim API here (see the header). For
    // Vitalera this is the gated "Monitoreds (Patients)" / consent endpoints.
    if (err) {
        *err = vendor_id + ": ownership verification is not implemented yet "
               "(needs the vendor's consent/claim API)";
    }
    return false;
}

}}  // namespace mirobody::health
