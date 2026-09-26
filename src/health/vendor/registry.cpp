#include "health/vendor/registry.hpp"

namespace mirobody { namespace vendor {

// Per-vendor factories. Each is defined in its own <id>.cpp under
// src/health/vendor/ (sorted into platform/ , phone/ , device/), holding the
// concrete (anonymous-namespace) class and its VendorInfo. Declared here rather
// than in a header because nothing else constructs them directly.
std::unique_ptr<Vendor> make_ehr(const VendorConfig&);

namespace {

typedef std::unique_ptr<Vendor> (*Factory)(const VendorConfig&);

struct Registration {
    const char* id;
    Factory     make;
};

// The server-side connectors (B2B aggregators, device-brand clouds, Huawei's
// cloud) moved out with the rest of the server: they need OAuth client secrets
// a phone app cannot hold. On the phone, device data arrives through the
// platform health store instead (HealthKit / Health Connect / HMS).
const Registration kVendors[] = {
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
