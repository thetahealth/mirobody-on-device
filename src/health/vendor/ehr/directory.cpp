#include "health/vendor/ehr/directory.hpp"

#include "client/http_client.hpp"

#include <rapidjson/document.h>

#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace mirobody { namespace vendor {

//------------------------------------------------------------------------------
// Default source URLs. Configurable because the published paths move; see the
// per-class notes in directory.hpp for each one's verification status.
//------------------------------------------------------------------------------

// ONC Lantern endpoint export. Confirm the current path/shape (the dashboard is a
// SPA; the back end exposes the harvested data) before relying on this in prod.
const char* const LanternDirectory::kDefaultUrl =
    "https://lantern.healthit.gov/api/endpoints/list";

// Oracle Health (Cerner Millennium) published patient-facing FHIR R4 Endpoint
// bundle (raw GitHub). The repo moved to the oracle-samples org; raw GitHub does
// not follow the rename redirect, so the cerner/* path 404s — use oracle-samples.
const char* const OracleHealthDirectory::kDefaultUrl =
    "https://raw.githubusercontent.com/oracle-samples/ignite-endpoints/main/"
    "oracle_health_fhir_endpoints/millennium_patient_r4_endpoints.json";

//------------------------------------------------------------------------------

FhirEndpointBundleDirectory::FhirEndpointBundleDirectory(std::string source,
                                                         std::string url,
                                                         int timeout_ms)
    : source_(std::move(source)), url_(std::move(url)), timeout_ms_(timeout_ms) {}

bool FhirEndpointBundleDirectory::fetch(std::vector<EhrEndpoint>& out, std::string& err) {
    client::HttpResponse res = client::HttpClient().get(url_, timeout_ms_);
    if (res.status < 200 || res.status >= 300) {
        err = source_ + ": GET " + url_ + " failed (HTTP " + std::to_string(res.status) + ")";
        return false;
    }

    rapidjson::Document doc;
    doc.Parse(res.body.c_str(), res.body.size());
    if (doc.HasParseError() || !doc.IsObject()) {
        err = source_ + ": response is not a JSON object";
        return false;
    }

    // FHIR Bundle of Endpoint (+ Organization) resources. Two shapes in the wild:
    //   * Epic — the org name is on the Endpoint (name / managingOrganization.display).
    //   * Oracle Health — the Endpoint has only an address; the name lives on a
    //     separate Organization resource whose `endpoint[].reference` points back
    //     at it ("Endpoint/<id>"). So we make one pass collecting both, then join.
    auto entries = doc.FindMember("entry");
    if (entries == doc.MemberEnd() || !entries->value.IsArray()) {
        err = source_ + ": no Bundle `entry` array";
        return false;
    }

    const std::size_t base = out.size();
    std::unordered_map<std::string, std::size_t> by_endpoint_id;  // Endpoint id -> index in out
    std::unordered_map<std::string, std::string> name_by_endpoint_id;  // from Organizations

    // The trailing id of a "Endpoint/<id>" (or "#<id>", or absolute URL) reference.
    auto ref_id = [](const std::string& ref) -> std::string {
        std::size_t p = ref.find_last_of("/#");
        return p == std::string::npos ? ref : ref.substr(p + 1);
    };

    for (const auto& entry : entries->value.GetArray()) {
        if (!entry.IsObject()) continue;
        auto r = entry.FindMember("resource");
        const rapidjson::Value& resv = (r != entry.MemberEnd() && r->value.IsObject())
                                           ? r->value : entry;   // tolerate bare resources

        auto type = resv.FindMember("resourceType");
        const std::string rt = (type != resv.MemberEnd() && type->value.IsString())
                                   ? type->value.GetString() : "";

        if (rt == "Endpoint" || rt.empty()) {
            auto addr = resv.FindMember("address");
            if (addr == resv.MemberEnd() || !addr->value.IsString()) continue;
            EhrEndpoint ep;
            ep.fhir_base_url = addr->value.GetString();
            auto name = resv.FindMember("name");
            if (name != resv.MemberEnd() && name->value.IsString()) {
                ep.name = name->value.GetString();
            } else {
                auto org = resv.FindMember("managingOrganization");
                if (org != resv.MemberEnd() && org->value.IsObject()) {
                    auto disp = org->value.FindMember("display");
                    if (disp != org->value.MemberEnd() && disp->value.IsString()) {
                        ep.name = disp->value.GetString();
                    }
                }
            }
            auto id = resv.FindMember("id");
            if (id != resv.MemberEnd() && id->value.IsString()) {
                by_endpoint_id[id->value.GetString()] = out.size();
            }
            out.push_back(std::move(ep));
        } else if (rt == "Organization") {
            auto name = resv.FindMember("name");
            if (name == resv.MemberEnd() || !name->value.IsString()) continue;
            auto eps = resv.FindMember("endpoint");
            if (eps == resv.MemberEnd() || !eps->value.IsArray()) continue;
            for (const auto& ref : eps->value.GetArray()) {
                if (!ref.IsObject()) continue;
                auto refstr = ref.FindMember("reference");
                if (refstr != ref.MemberEnd() && refstr->value.IsString()) {
                    name_by_endpoint_id[ref_id(refstr->value.GetString())] = name->value.GetString();
                }
            }
        }
    }

    // Join Organization names onto endpoints that had none of their own.
    for (const auto& kv : by_endpoint_id) {
        if (out[kv.second].name.empty()) {
            auto n = name_by_endpoint_id.find(kv.first);
            if (n != name_by_endpoint_id.end()) out[kv.second].name = n->second;
        }
    }

    if (out.size() == base) {
        err = source_ + ": Bundle contained no Endpoint resources with an address";
        return false;
    }
    return true;
}

//------------------------------------------------------------------------------

LanternDirectory::LanternDirectory(std::string url)
    : FhirEndpointBundleDirectory("lantern", std::move(url)) {}

OracleHealthDirectory::OracleHealthDirectory(std::string url)
    : FhirEndpointBundleDirectory("oracle_health", std::move(url)) {}

//------------------------------------------------------------------------------

std::vector<EhrEndpoint> fetch_all(const std::vector<EhrDirectory*>& sources,
                                   std::vector<std::string>* errors) {
    std::vector<EhrEndpoint> merged;
    std::unordered_set<std::string> seen;   // dedup by base URL, first wins
    for (EhrDirectory* src : sources) {
        if (!src) continue;
        std::vector<EhrEndpoint> batch;
        std::string err;
        if (!src->fetch(batch, err)) {
            if (errors) errors->push_back(err);
            continue;
        }
        for (EhrEndpoint& ep : batch) {
            if (seen.insert(ep.fhir_base_url).second) merged.push_back(std::move(ep));
        }
    }
    return merged;
}

}}
