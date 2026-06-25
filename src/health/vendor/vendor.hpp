#pragma once

// Health-data vendor abstraction. Mirrors the role storage::Storage plays for
// object stores: a single interface the rest of the server talks to, with the
// concrete vendor chosen at runtime by id (see src/health/vendor/registry.hpp).
//
// Each vendor is one of the platforms profiled in README.md — a health-data
// aggregator that brokers wearable, lab-diagnostic, and/or clinical-EHR data on
// a consumer's behalf. The set is broad on purpose: a deployment may pull
// wearable streams from one vendor and clinical records from another.
//
// STATUS: scaffold. Every concrete vendor today is a stub — it carries the
// VendorInfo metadata extracted from README.md (positioning, compliance posture,
// integration style, data domains) but its network operations throw
// VendorError("... not implemented"). The metadata is real and queryable now;
// the transport is filled in per vendor as the actual API reference and
// credentials become available. See the <id>.cpp under src/health/vendor/ (sorted
// into platform/ , phone/ , and device/) for where each one's real endpoints,
// auth flow, and response mapping go.

#include "compat/cxx11.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace mirobody { namespace vendor {

class Vendor;

//------------------------------------------------------------------------------

// Thrown on any non-success outcome: an unimplemented stub operation, a
// transport failure, a non-2xx response, or misconfiguration.
class VendorError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

//------------------------------------------------------------------------------
// Metadata enums (derived from the README.md comparison matrix)
//------------------------------------------------------------------------------

// Primary regulatory / hosting orientation of the platform.
enum class Region {
    Global,      // markets globally; no single jurisdiction emphasized
    US,          // U.S.-first (HIPAA framework, QHIN access)
    EU,          // Europe-first (GDPR, local hosting)
    SelfHosted,  // open-source self-hosted; compliance is the deployer's job
};

// Categories of health data a vendor brokers.
enum class DataDomain {
    Activity,
    Sleep,
    HeartRate,
    Glucose,
    Nutrition,
    BodyMetrics,   // weight, BMI, blood pressure, SpO2, …
    Labs,          // diagnostic lab panels
    Clinical,      // clinical EHR records (encounters, conditions, meds)
};

// How a vendor exposes its API. A platform may support several at once.
enum class Integration {
    Rest,         // REST / RESTful HTTP API
    Sdk,          // mobile / cross-platform client SDK
    Webhook,      // asynchronous push to a developer endpoint
    WebSocket,    // real-time streaming
    Fhir,         // FHIR (R4) clinical-standard payloads
    Hl7,          // HL7 message exchange
    WhiteLabel,   // embeddable white-label / hosted UI
};

inline const char* to_string(Region r) {
    switch (r) {
        case Region::Global:     return "global";
        case Region::US:         return "us";
        case Region::EU:         return "eu";
        case Region::SelfHosted: return "self-hosted";
    }
    return "unknown";
}

inline const char* to_string(DataDomain d) {
    switch (d) {
        case DataDomain::Activity:    return "activity";
        case DataDomain::Sleep:       return "sleep";
        case DataDomain::HeartRate:   return "heart_rate";
        case DataDomain::Glucose:     return "glucose";
        case DataDomain::Nutrition:   return "nutrition";
        case DataDomain::BodyMetrics: return "body_metrics";
        case DataDomain::Labs:        return "labs";
        case DataDomain::Clinical:    return "clinical";
    }
    return "unknown";
}

inline const char* to_string(Integration i) {
    switch (i) {
        case Integration::Rest:       return "rest";
        case Integration::Sdk:        return "sdk";
        case Integration::Webhook:    return "webhook";
        case Integration::WebSocket:  return "websocket";
        case Integration::Fhir:       return "fhir";
        case Integration::Hl7:        return "hl7";
        case Integration::WhiteLabel: return "white_label";
    }
    return "unknown";
}

// Parse a DataDomain from its to_string() spelling. Returns false on no match.
bool parse_domain(const std::string& s, DataDomain& out);

//------------------------------------------------------------------------------
// VendorInfo
//------------------------------------------------------------------------------

// Static facts about a vendor, lifted from the README.md matrix. Populated by
// each concrete vendor's constructor and returned by Vendor::info(); available
// without credentials or a network call, so the registry can describe the whole
// field for selection / comparison.
struct VendorInfo {
    std::string id;             // stable lowercase key, e.g. "terra"
    std::string display_name;   // "Terra API"
    std::string positioning;    // core positioning
    std::string target_customers;
    std::string data_source_coverage;
    std::string integration_method;   // human-readable integration summary
    std::string compliance_summary;   // human-readable compliance summary
    std::string differentiator;       // key differentiator
    std::string docs_url;             // official developer docs (from Sources)

    Region region = Region::Global;
    bool open_source = false;

    // Structured, queryable view of the same posture. `compliance` holds short
    // credential tags ("HIPAA", "GDPR", "SOC2_TYPE_II", "ISO_27001", "HITRUST",
    // "SaMD", "QHIN"); empty means none publicly disclosed in the report.
    std::vector<std::string> compliance;
    std::vector<DataDomain>  domains;
    std::vector<Integration> integrations;
};

//------------------------------------------------------------------------------
// VendorConfig
//------------------------------------------------------------------------------

// Per-vendor credentials and connection settings. Most of these platforms use a
// bearer/API key or an OAuth client pair; `base_url` overrides the default API
// host for sandbox / region endpoints. Wiring into the central mirobody::Config
// is intentionally deferred — see from_env() for the convention used until then.
struct VendorConfig {
    std::string api_key;        // bearer / API key (most REST vendors)
    std::string client_id;      // OAuth client id (consumer-mediated consent)
    std::string client_secret;  // OAuth client secret
    std::string base_url;       // API host override; empty => vendor default

    bool configured() const {
        return !api_key.empty() || (!client_id.empty() && !client_secret.empty());
    }

    // Build a config from environment variables, by convention:
    //   MIROBODY_VENDOR_<ID>_API_KEY / _CLIENT_ID / _CLIENT_SECRET / _BASE_URL
    // where <ID> is the vendor id upper-cased (e.g. HUMAN_API). Missing vars are
    // left empty. Defined in vendor.cpp.
    static VendorConfig from_env(const std::string& id);
};

//------------------------------------------------------------------------------
// Vendor
//------------------------------------------------------------------------------

// A health-data vendor client. `user_id` is the platform's identifier for the
// end user whose data is being brokered (the value returned by the consent
// flow). Time bounds are ISO-8601 strings. Data-returning operations hand back
// the vendor's JSON as a string for now; a normalized model is a later step.
//
// All operations may throw VendorError. In the current scaffold every concrete
// vendor inherits the VendorBase stubs, which throw "not implemented".
class Vendor {
public:
    virtual ~Vendor() = default;

    // Static metadata for this vendor (never throws, no network).
    virtual const VendorInfo& info() const = 0;

    // Begin consumer-mediated consent: the URL to redirect a user to so they can
    // authorize access (device account or hospital patient portal). `state` is
    // echoed back to `redirect_uri` for CSRF protection / correlation.
    virtual std::string authorize_url(const std::string& redirect_uri,
                                      const std::string& state) = 0;

    // The data sources (device brands / labs / health systems) a connected user
    // can link, as the vendor's JSON.
    virtual std::string list_providers() = 0;

    // Fetch `domain` data for `user_id` over [start_iso, end_iso], as the
    // vendor's JSON. Empty bounds mean "the vendor's default window".
    virtual std::string fetch(const std::string& user_id,
                              DataDomain domain,
                              const std::string& start_iso,
                              const std::string& end_iso) = 0;

    // Validate and parse an inbound webhook delivery; returns the parsed event
    // as JSON. `raw_headers` carries the signature header(s) the vendor signs
    // with. Vendors without webhooks may throw VendorError.
    virtual std::string handle_webhook(const std::string& raw_headers,
                                       const std::string& body) = 0;

    // Revoke this user's authorization / disconnect them from the vendor.
    virtual void revoke(const std::string& user_id) = 0;
};

//------------------------------------------------------------------------------
// VendorBase
//------------------------------------------------------------------------------

// Common base for every concrete vendor. Stores the VendorInfo + VendorConfig
// and implements info(); every network operation defaults to throwing
// VendorError via not_implemented(). A concrete vendor overrides the operations
// it has implemented and leaves the rest as stubs, so the scaffold compiles and
// runs (reporting "not implemented") before any transport exists.
class VendorBase : public Vendor {
public:
    VendorBase(VendorInfo info, VendorConfig config)
        : info_(std::move(info)), config_(std::move(config)) {}

    const VendorInfo& info() const override { return info_; }

    std::string authorize_url(const std::string&, const std::string&) override {
        return not_implemented("authorize_url");
    }
    std::string list_providers() override {
        return not_implemented("list_providers");
    }
    std::string fetch(const std::string&, DataDomain,
                      const std::string&, const std::string&) override {
        return not_implemented("fetch");
    }
    std::string handle_webhook(const std::string&, const std::string&) override {
        return not_implemented("handle_webhook");
    }
    void revoke(const std::string&) override {
        not_implemented("revoke");
    }

protected:
    const VendorConfig& config() const { return config_; }

    // Throw a uniform "not implemented" VendorError naming the vendor and op.
    // Returns std::string only so the value-returning stubs above can `return`
    // it in a single expression; it never actually returns.
    std::string not_implemented(const char* op) const {
        throw VendorError(info_.id + ": " + op + " not implemented (stub) — see " +
                          info_.id + ".cpp under src/health/vendor/");
    }

    VendorInfo   info_;
    VendorConfig config_;
};

}}
