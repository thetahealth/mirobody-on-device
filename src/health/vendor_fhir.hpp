#pragma once

// Map vendor-native fetch() JSON to FHIR R4 Observation resources.
//
// The device-brand clients (oura, whoop, dexcom, …) return each vendor's own JSON,
// not FHIR; WeChat WeRun likewise returns its own decrypted step JSON. This is the
// seam that turns that JSON into FHIR R4 `Observation` resource bodies, so the data
// lands in the SAME FHIR write path the on-device apps and the EHR connect flow use
// (fhir::FhirStore). Callers: VendorService's /vendors/{id}/sync (device brands) and
// WeRunService's /wechat/werun (WeChat steps).
//
// One mapper per (vendor, domain). A reading is mapped only when it has a confident
// LOINC code + UCUM unit; metrics without one (sleep scores, strain, HRV) are
// deferred — left unmapped and documented — rather than mapped to a wrong code, the
// same honesty rule the vendor clients follow. Currently mapped:
//
//   | vendor | domain     | reading                | LOINC   | unit          |
//   | oura   | HeartRate  | data[].bpm             | 8867-4  | /min          |
//   | oura   | Activity   | data[].steps           | 55423-8 | {steps}       |
//   | whoop  | HeartRate  | recovery resting_hr    | 40443-4 | /min          |
//   | whoop  | HeartRate  | recovery spo2_percent  | 59408-5 | %             |
//   | dexcom | Glucose    | records[].value        | 2339-0  | mg/dL | mmol/L |
//   | werun  | (steps)    | stepInfoList[].step    | 55423-8 | {steps}       |

#include "health/vendor/vendor.hpp"

#include <string>
#include <vector>

namespace mirobody { namespace health {

// Convert a vendor's fetch() JSON for `domain` into zero or more FHIR R4
// Observation JSON strings. `subject_ref` is recorded as each Observation's subject
// (e.g. "Patient/42"; omitted when empty). Returns empty — never throws — when the
// vendor/domain is unmapped, the JSON does not parse, or it carries no mappable
// readings.
std::vector<std::string> vendor_json_to_observations(
    const std::string& vendor_id, vendor::DataDomain domain,
    const std::string& vendor_json, const std::string& subject_ref);

}}  // namespace mirobody::health
