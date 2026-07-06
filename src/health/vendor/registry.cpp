#include "health/vendor/registry.hpp"

namespace mirobody { namespace vendor {

// Per-vendor factories. Each is defined in its own <id>.cpp under
// src/health/vendor/ (sorted into platform/ , phone/ , device/), holding the
// concrete (anonymous-namespace) class and its VendorInfo. Declared here rather
// than in a header because nothing else constructs them directly.
std::unique_ptr<Vendor> make_rook(const VendorConfig&);
std::unique_ptr<Vendor> make_spike(const VendorConfig&);
std::unique_ptr<Vendor> make_terra(const VendorConfig&);
std::unique_ptr<Vendor> make_junction(const VendorConfig&);
std::unique_ptr<Vendor> make_wefitter(const VendorConfig&);
std::unique_ptr<Vendor> make_lexisnexis(const VendorConfig&);
std::unique_ptr<Vendor> make_thryve(const VendorConfig&);
std::unique_ptr<Vendor> make_validic(const VendorConfig&);
std::unique_ptr<Vendor> make_human_api(const VendorConfig&);
std::unique_ptr<Vendor> make_vitalera(const VendorConfig&);
std::unique_ptr<Vendor> make_open_wearables(const VendorConfig&);
std::unique_ptr<Vendor> make_redox(const VendorConfig&);
std::unique_ptr<Vendor> make_particle_health(const VendorConfig&);
std::unique_ptr<Vendor> make_healthconnect(const VendorConfig&);
std::unique_ptr<Vendor> make_metriport(const VendorConfig&);
std::unique_ptr<Vendor> make_huawei(const VendorConfig&);
std::unique_ptr<Vendor> make_fitbit(const VendorConfig&);
std::unique_ptr<Vendor> make_withings(const VendorConfig&);
std::unique_ptr<Vendor> make_garmin(const VendorConfig&);
std::unique_ptr<Vendor> make_dexcom(const VendorConfig&);
std::unique_ptr<Vendor> make_oura(const VendorConfig&);
std::unique_ptr<Vendor> make_whoop(const VendorConfig&);
std::unique_ptr<Vendor> make_polar(const VendorConfig&);
std::unique_ptr<Vendor> make_ehr(const VendorConfig&);

namespace {

typedef std::unique_ptr<Vendor> (*Factory)(const VendorConfig&);

struct Registration {
    const char* id;
    Factory     make;
};

// Matrix order matches the README.md comparison table.
const Registration kVendors[] = {
    {"rook",            &make_rook},
    {"spike",           &make_spike},
    {"terra",           &make_terra},
    {"junction",        &make_junction},
    {"wefitter",        &make_wefitter},
    {"lexisnexis",      &make_lexisnexis},
    {"thryve",          &make_thryve},
    {"validic",         &make_validic},
    {"human_api",       &make_human_api},
    {"vitalera",        &make_vitalera},
    {"open_wearables",  &make_open_wearables},
    {"redox",           &make_redox},
    {"particle_health", &make_particle_health},
    {"healthconnect",   &make_healthconnect},
    {"metriport",       &make_metriport},
    // Device-native platform with a cloud REST API; beyond the original report's
    // 15-platform comparison matrix (Apple/Google/Samsung/Xiaomi are on-device
    // only — see src/health/README.md).
    {"huawei",          &make_huawei},
    // Device *brands* with their own OAuth REST APIs — direct clients for talking
    // to them without an aggregator (Fitbit/Withings pull; Garmin is push-based).
    {"fitbit",          &make_fitbit},
    {"withings",        &make_withings},
    {"garmin",          &make_garmin},
    {"dexcom",          &make_dexcom},
    {"oura",            &make_oura},
    {"whoop",           &make_whoop},
    {"polar",           &make_polar},
    // Direct EHR-system access via SMART on FHIR — one client for every certified
    // EHR (Epic, Oracle Health/Cerner, …), tenant base_url from ehr/directory.hpp.
    {"ehr",             &make_ehr},
};

}

//------------------------------------------------------------------------------

std::vector<std::string> vendor_ids() {
    std::vector<std::string> ids;
    for (const Registration& r : kVendors) ids.push_back(r.id);
    return ids;
}

std::vector<VendorInfo> all_vendor_info() {
    std::vector<VendorInfo> out;
    const VendorConfig empty;
    for (const Registration& r : kVendors) out.push_back(r.make(empty)->info());
    return out;
}

std::unique_ptr<Vendor> open_vendor(const std::string& id, const VendorConfig& config) {
    for (const Registration& r : kVendors) {
        if (id == r.id) return r.make(config);
    }
    throw VendorError("unknown vendor id: " + id +
                      " (run `vendor list` for the registered ids)");
}

}}
