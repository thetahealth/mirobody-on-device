#include "health/vendor_fhir.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace mirobody { namespace health {
namespace {

// Read a numeric member (int/uint/double) as double. False when absent/non-number.
bool num_member(const rapidjson::Value& v, const char* key, double& out) {
    if (!v.IsObject() || !v.HasMember(key)) return false;
    const rapidjson::Value& m = v[key];
    if (!m.IsNumber()) return false;
    out = m.GetDouble();
    return true;
}

// Read a string member; "" when absent or not a string.
std::string str_member(const rapidjson::Value& v, const char* key) {
    if (!v.IsObject() || !v.HasMember(key)) return std::string();
    const rapidjson::Value& m = v[key];
    if (m.IsString()) return std::string(m.GetString(), m.GetStringLength());
    return std::string();
}

// Format Unix epoch seconds as an ISO-8601 UTC instant (WeRun timestamps are epoch
// seconds; FHIR effectiveDateTime wants an ISO string).
std::string unix_to_iso(std::int64_t secs) {
    std::time_t t = static_cast<std::time_t>(secs);
    std::tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

// Serialize one FHIR R4 Observation. `integral` prints the value as an integer
// (steps / bpm read cleaner than 60.0). `effective`/`subject` are omitted when empty.
std::string build_observation(const char* loinc, const char* display, const char* category,
                              double value, bool integral, const char* unit, const char* ucum,
                              const std::string& effective, const std::string& subject) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("resourceType"); w.String("Observation");
    w.Key("status");       w.String("final");

    w.Key("category");
    w.StartArray(); w.StartObject();
        w.Key("coding");
        w.StartArray(); w.StartObject();
            w.Key("system"); w.String("http://terminology.hl7.org/CodeSystem/observation-category");
            w.Key("code");   w.String(category);
        w.EndObject(); w.EndArray();
    w.EndObject(); w.EndArray();

    w.Key("code");
    w.StartObject();
        w.Key("coding");
        w.StartArray(); w.StartObject();
            w.Key("system");  w.String("http://loinc.org");
            w.Key("code");    w.String(loinc);
            w.Key("display"); w.String(display);
        w.EndObject(); w.EndArray();
    w.EndObject();

    if (!subject.empty()) {
        w.Key("subject");
        w.StartObject();
            w.Key("reference");
            w.String(subject.data(), static_cast<rapidjson::SizeType>(subject.size()));
        w.EndObject();
    }
    if (!effective.empty()) {
        w.Key("effectiveDateTime");
        w.String(effective.data(), static_cast<rapidjson::SizeType>(effective.size()));
    }

    w.Key("valueQuantity");
    w.StartObject();
        w.Key("value");
        if (integral) w.Int64(static_cast<std::int64_t>(value));
        else          w.Double(value);
        w.Key("unit");   w.String(unit);
        w.Key("system"); w.String("http://unitsofmeasure.org");
        w.Key("code");   w.String(ucum);
    w.EndObject();

    w.EndObject();
    return std::string(sb.GetString(), sb.GetSize());
}

//------------------------------------------------------------------------------

// Oura v2 usercollection responses are { "data": [ {...}, ... ], "next_token": ... }.
void map_oura(vendor::DataDomain domain, const rapidjson::Value& data,
              const std::string& subject, std::vector<std::string>& out) {
    for (const rapidjson::Value& it : data.GetArray()) {
        if (!it.IsObject()) continue;
        double v = 0.0;
        switch (domain) {
            case vendor::DataDomain::HeartRate:
                // heartrate series: { bpm, timestamp }
                if (num_member(it, "bpm", v)) {
                    out.push_back(build_observation(
                        "8867-4", "Heart rate", "vital-signs", v, /*integral=*/true,
                        "beats/minute", "/min", str_member(it, "timestamp"), subject));
                }
                break;
            case vendor::DataDomain::Activity:
                // daily_activity: { day, steps, ... }
                if (num_member(it, "steps", v)) {
                    out.push_back(build_observation(
                        "55423-8", "Number of steps in 24 hour Measured", "activity", v,
                        /*integral=*/true, "steps", "{steps}", str_member(it, "day"), subject));
                }
                break;
            default:
                // Sleep (daily_sleep) is a proprietary score with no standard LOINC —
                // deferred (the detailed `sleep` endpoint carries durations to map later).
                return;
        }
    }
}

// WHOOP v2 collection responses are { "records": [ {...}, ... ], "next_token": ... }.
void map_whoop(vendor::DataDomain domain, const rapidjson::Value& records,
               const std::string& subject, std::vector<std::string>& out) {
    if (domain != vendor::DataDomain::HeartRate) {
        // Sleep (durations) and Activity (strain/cycle) mapping is deferred; only the
        // Recovery record carries readings with confident LOINC codes.
        return;
    }
    for (const rapidjson::Value& rec : records.GetArray()) {
        if (!rec.IsObject()) continue;
        rapidjson::Value::ConstMemberIterator sc = rec.FindMember("score");
        if (sc == rec.MemberEnd() || !sc->value.IsObject()) continue;
        const std::string when = str_member(rec, "created_at");

        double rhr = 0.0;
        if (num_member(sc->value, "resting_heart_rate", rhr)) {
            out.push_back(build_observation(
                "40443-4", "Heart rate --resting", "vital-signs", rhr, /*integral=*/true,
                "beats/minute", "/min", when, subject));
        }
        double spo2 = 0.0;
        if (num_member(sc->value, "spo2_percentage", spo2)) {
            out.push_back(build_observation(
                "59408-5", "Oxygen saturation in Arterial blood by Pulse oximetry", "vital-signs",
                spo2, /*integral=*/false, "%", "%", when, subject));
        }
    }
}

// Dexcom v3 egvs responses are { "unit": "mg/dL", ..., "records": [ { systemTime,
// displayTime, value, ... } ] }. The glucose unit is deployment-region-dependent
// ("mg/dL" US / "mmol/L" elsewhere), carried at the top level and passed through
// verbatim (both are valid UCUM), defaulting to mg/dL.
void map_dexcom(vendor::DataDomain domain, const rapidjson::Value& records,
                const std::string& unit, const std::string& subject,
                std::vector<std::string>& out) {
    if (domain != vendor::DataDomain::Glucose) return;  // Dexcom brokers only glucose
    const std::string u = unit.empty() ? std::string("mg/dL") : unit;
    for (const rapidjson::Value& rec : records.GetArray()) {
        if (!rec.IsObject()) continue;
        double v = 0.0;
        if (!num_member(rec, "value", v)) continue;
        std::string when = str_member(rec, "systemTime");
        if (when.empty()) when = str_member(rec, "displayTime");
        out.push_back(build_observation(
            "2339-0", "Glucose [Mass/volume] in Blood", "laboratory", v, /*integral=*/false,
            u.c_str(), u.c_str(), when, subject));
    }
}

// WeChat WeRun decrypted payload is { "stepInfoList": [ { timestamp, step }, ... ],
// "watermark": {...} } — one entry per day, timestamp in epoch seconds. Steps only.
void map_werun(const rapidjson::Value& step_info, const std::string& subject,
               std::vector<std::string>& out) {
    for (const rapidjson::Value& it : step_info.GetArray()) {
        if (!it.IsObject()) continue;
        double step = 0.0;
        if (!num_member(it, "step", step)) continue;
        double ts = 0.0;
        const std::string when = num_member(it, "timestamp", ts)
                                     ? unix_to_iso(static_cast<std::int64_t>(ts)) : std::string();
        out.push_back(build_observation(
            "55423-8", "Number of steps in 24 hour Measured", "activity", step,
            /*integral=*/true, "steps", "{steps}", when, subject));
    }
}

}  // namespace

//------------------------------------------------------------------------------

std::vector<std::string> vendor_json_to_observations(
    const std::string& vendor_id, vendor::DataDomain domain,
    const std::string& vendor_json, const std::string& subject_ref) {
    std::vector<std::string> out;
    rapidjson::Document doc;
    doc.Parse(vendor_json.c_str(), vendor_json.size());
    if (doc.HasParseError() || !doc.IsObject()) return out;

    if (vendor_id == "oura") {
        rapidjson::Value::ConstMemberIterator data = doc.FindMember("data");
        if (data != doc.MemberEnd() && data->value.IsArray()) {
            map_oura(domain, data->value, subject_ref, out);
        }
    } else if (vendor_id == "whoop") {
        rapidjson::Value::ConstMemberIterator recs = doc.FindMember("records");
        if (recs != doc.MemberEnd() && recs->value.IsArray()) {
            map_whoop(domain, recs->value, subject_ref, out);
        }
    } else if (vendor_id == "dexcom") {
        rapidjson::Value::ConstMemberIterator recs = doc.FindMember("records");
        if (recs != doc.MemberEnd() && recs->value.IsArray()) {
            std::string unit;
            rapidjson::Value::ConstMemberIterator u = doc.FindMember("unit");
            if (u != doc.MemberEnd() && u->value.IsString()) {
                unit.assign(u->value.GetString(), u->value.GetStringLength());
            }
            map_dexcom(domain, recs->value, unit, subject_ref, out);
        }
    } else if (vendor_id == "werun") {
        // WeChat WeRun (steps only); domain is ignored (always Activity).
        rapidjson::Value::ConstMemberIterator si = doc.FindMember("stepInfoList");
        if (si != doc.MemberEnd() && si->value.IsArray()) {
            map_werun(si->value, subject_ref, out);
        }
    }
    return out;
}

}}  // namespace mirobody::health
