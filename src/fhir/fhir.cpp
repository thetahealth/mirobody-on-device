#include "fhir/fhir.hpp"

namespace mirobody { namespace fhir {

const char* const kFhirVersionR4  = "4.0.1";
const char* const kFhirVersionR4B = "4.3.0";
const char* const kFhirJsonMime   = "application/fhir+json";

bool is_supported_fhir_version(const std::string& v) {
    return v == kFhirVersionR4 || v == kFhirVersionR4B;
}

const std::vector<std::string>& known_resource_types() {
    static const std::vector<std::string> v = {
        "AllergyIntolerance", "Condition", "Encounter", "Immunization",
        "Location", "Medication", "MedicationRequest", "MedicationStatement",
        "Observation", "Organization", "Patient", "Practitioner",
        "PractitionerRole", "Procedure",
    };
    return v;
}

bool is_well_formed_resource_type(const std::string& t) {
    if (t.empty()) return false;
    if (!(t[0] >= 'A' && t[0] <= 'Z')) return false;
    for (size_t i = 1; i < t.size(); ++i) {
        char c = t[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
            return false;
    }
    return true;
}

bool is_valid_fhir_id(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (size_t i = 0; i < id.size(); ++i) {
        char c = id[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-'))
            return false;
    }
    return true;
}

}}  // namespace mirobody::fhir
