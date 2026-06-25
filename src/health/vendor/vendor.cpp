#include "health/vendor/vendor.hpp"

#include <cctype>
#include <cstdlib>

namespace mirobody { namespace vendor {

//------------------------------------------------------------------------------

bool parse_domain(const std::string& s, DataDomain& out) {
    static const DataDomain kAll[] = {
        DataDomain::Activity, DataDomain::Sleep, DataDomain::HeartRate,
        DataDomain::Glucose, DataDomain::Nutrition, DataDomain::BodyMetrics,
        DataDomain::Labs, DataDomain::Clinical,
    };
    for (DataDomain d : kAll) {
        if (s == to_string(d)) { out = d; return true; }
    }
    return false;
}

//------------------------------------------------------------------------------

namespace {

// Uppercase an id for the env-var convention: "human_api" -> "HUMAN_API".
std::string env_key(const std::string& id, const char* suffix) {
    std::string key = "MIROBODY_VENDOR_";
    for (char c : id) key += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    key += suffix;
    return key;
}

std::string getenv_str(const std::string& key) {
    const char* v = std::getenv(key.c_str());
    return v ? std::string(v) : std::string();
}

}

//------------------------------------------------------------------------------

VendorConfig VendorConfig::from_env(const std::string& id) {
    VendorConfig cfg;
    cfg.api_key       = getenv_str(env_key(id, "_API_KEY"));
    cfg.client_id     = getenv_str(env_key(id, "_CLIENT_ID"));
    cfg.client_secret = getenv_str(env_key(id, "_CLIENT_SECRET"));
    cfg.base_url      = getenv_str(env_key(id, "_BASE_URL"));
    return cfg;
}

}}
