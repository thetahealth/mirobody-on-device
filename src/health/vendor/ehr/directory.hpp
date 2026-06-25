#pragma once

// EHR endpoint directories — discovering the per-tenant FHIR `base_url` the
// generic SMART-on-FHIR client (ehr.cpp) needs. Under the US ONC Cures Act every
// certified Health IT developer publishes its FHIR "Service Base URL" list as a
// public FHIR R4 `Bundle` of `Endpoint` resources (no auth). This module fetches
// and parses those lists behind one interface, so a national aggregator and a
// single-vendor list are interchangeable to callers.
//
// Sources (verified 2026-06):
//   * ONC Lantern — the national aggregator that harvests every certified
//     developer's published list (https://lantern.healthit.gov, OSS back end at
//     github.com/onc-healthit/lantern-back-end). Broadest coverage.
//   * Oracle Health / Cerner — the vendor's own published FHIR Endpoint bundle
//     (github.com/cerner/ignite-endpoints, actively maintained). Single-vendor.
// Epic publishes the same shape at open.epic.com/Endpoints/R4 (add a third source
// the same way). The exact source URLs are configurable because the published
// paths move; the parser targets the standard FHIR Endpoint Bundle shape.

#include <string>
#include <vector>

namespace mirobody { namespace vendor {

// One discovered tenant: a human-readable organization name and its FHIR R4
// service base URL (the value to feed into VendorConfig::base_url for `ehr`).
struct EhrEndpoint {
    std::string name;
    std::string fhir_base_url;
};

//------------------------------------------------------------------------------

// One directory source. Implementations fetch their list and parse it into
// endpoints; a national aggregator and a single-vendor list look identical here.
class EhrDirectory {
public:
    virtual ~EhrDirectory() = default;

    // Short source label for logging/UX, e.g. "lantern" / "oracle_health".
    virtual const char* source_name() const = 0;

    // Fetch the directory and APPEND parsed endpoints to `out`. Returns false and
    // sets `err` on a transport / parse failure, leaving `out` unchanged.
    virtual bool fetch(std::vector<EhrEndpoint>& out, std::string& err) = 0;
};

//------------------------------------------------------------------------------

// Fetches a URL returning a FHIR R4 Bundle of Endpoint resources and parses each
// entry's `address` (the base URL) and name. Epic and Oracle Health publish this
// exact shape; the concrete directories below differ only in their default URL.
class FhirEndpointBundleDirectory : public EhrDirectory {
public:
    FhirEndpointBundleDirectory(std::string source, std::string url, int timeout_ms = 30000);

    const char* source_name() const override { return source_.c_str(); }
    bool fetch(std::vector<EhrEndpoint>& out, std::string& err) override;

protected:
    std::string source_;
    std::string url_;
    int         timeout_ms_;
};

//------------------------------------------------------------------------------

// ONC Lantern (national aggregator). VERIFIED as ONC's project; its published
// export path/shape should be confirmed before production use — if Lantern serves
// its own JSON rather than a raw FHIR Endpoint Bundle, point this at a vendor
// Service Base URL list or add a Lantern-specific parser.
class LanternDirectory : public FhirEndpointBundleDirectory {
public:
    static const char* const kDefaultUrl;
    explicit LanternDirectory(std::string url = kDefaultUrl);
};

// Oracle Health (Cerner Millennium) patient-facing FHIR R4 Endpoint bundle,
// published at github.com/oracle-samples/ignite-endpoints (VERIFIED 2026-06; the
// org renamed from cerner/*). Endpoint names come from paired Organization
// resources, which the parser joins by reference.
class OracleHealthDirectory : public FhirEndpointBundleDirectory {
public:
    static const char* const kDefaultUrl;
    explicit OracleHealthDirectory(std::string url = kDefaultUrl);
};

//------------------------------------------------------------------------------

// Fetch every source in `sources`, merging results and de-duplicating by
// fhir_base_url (first occurrence wins). A source that fails is skipped; when
// `errors` is non-null, each failure's message is appended. The whole point of
// the shared interface: pass {LanternDirectory, OracleHealthDirectory, …} and get
// one merged provider list back.
std::vector<EhrEndpoint> fetch_all(const std::vector<EhrDirectory*>& sources,
                                   std::vector<std::string>* errors = nullptr);

}}
