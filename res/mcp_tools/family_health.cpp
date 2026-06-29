// MCP tool: family_health — read recent health records (FHIR Observations) for
// the caller or a care-circle member who has shared their health data.
//
// Auth required. The `member` argument resolves to a target user id: empty /
// "me" is the caller; a numeric id or a name/nickname/email matches someone who
// shared their health data (circle::health_shared_with), gated by
// circle::can_read_health. The tool then returns that subject's most recent
// Observations (code · value · time) for the model to summarize — the data half
// of "how is Mom doing?". The chat composer's "currently for" picker biases the
// model toward a member via a system-prompt hint (see res/agents/base.cpp); the
// model may also pick a member straight from the question.

#include "mcp/tool.hpp"

#include "circle/access.hpp"     // can_read_health, health_shared_with, HealthShare
#include "fhir/store.hpp"        // FhirStore, StoredResource

#include <rapidjson/document.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using namespace mirobody::mcp;
namespace circle = mirobody::circle;
namespace fhir   = mirobody::fhir;

std::string to_lower(std::string s) {
    for (std::size_t i = 0; i < s.size(); ++i) s[i] = static_cast<char>(std::tolower((unsigned char)s[i]));
    return s;
}
std::string trim(const std::string& s) {
    std::size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    std::size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}
bool all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (std::size_t i = 0; i < s.size(); ++i) if (!std::isdigit((unsigned char)s[i])) return false;
    return true;
}
std::string display_of(const circle::HealthShare& p) {
    return p.nickname.empty() ? p.email : p.nickname;
}

// Resolve `member` to a user id the caller may read, or -1. `label` gets a
// human name for the result payload.
std::int64_t resolve_subject(mirobody::database::Database& db, std::int64_t caller,
                             const std::string& member, std::string* label) {
    const std::string m  = trim(member);
    const std::string ml = to_lower(m);
    if (m.empty() || ml == "me" || ml == "self" || ml == "myself" || ml == "i" || ml == "my" || ml == "mine") {
        if (label) *label = "you";
        return caller;
    }
    std::vector<circle::HealthShare> people = circle::health_shared_with(db, caller);
    if (all_digits(m)) {
        const std::int64_t id = std::strtoll(m.c_str(), nullptr, 10);
        if (id == caller) { if (label) *label = "you"; return caller; }
        for (std::size_t i = 0; i < people.size(); ++i)
            if (people[i].user_id == id) { if (label) *label = display_of(people[i]); return id; }
        return -1;
    }
    std::int64_t exact = -1, sub = -1; int subCount = 0; std::string exactLabel, subLabel;
    for (std::size_t i = 0; i < people.size(); ++i) {
        const std::string nk = to_lower(people[i].nickname), em = to_lower(people[i].email);
        if ((!nk.empty() && nk == ml) || (!em.empty() && em == ml)) { exact = people[i].user_id; exactLabel = display_of(people[i]); }
        if ((!nk.empty() && nk.find(ml) != std::string::npos) || (!em.empty() && em.find(ml) != std::string::npos)) {
            sub = people[i].user_id; subLabel = display_of(people[i]); ++subCount;
        }
    }
    if (exact > 0)     { if (label) *label = exactLabel; return exact; }
    if (subCount == 1) { if (label) *label = subLabel;   return sub;   }
    return -1;
}

// Compact {code, value, time} for one Observation resource JSON.
void summarize_observation(const std::string& json, rapidjson::Value& out,
                           rapidjson::Document::AllocatorType& a) {
    rapidjson::Document d;
    if (d.Parse(json.c_str(), json.size()).HasParseError() || !d.IsObject()) return;

    std::string code;
    if (d.HasMember("code") && d["code"].IsObject()) {
        const rapidjson::Value& c = d["code"];
        if (c.HasMember("text") && c["text"].IsString()) code = c["text"].GetString();
        else if (c.HasMember("coding") && c["coding"].IsArray() && c["coding"].Size() > 0) {
            const rapidjson::Value& cd = c["coding"][0];
            if (cd.HasMember("display") && cd["display"].IsString())   code = cd["display"].GetString();
            else if (cd.HasMember("code") && cd["code"].IsString())    code = cd["code"].GetString();
        }
    }

    std::string value;
    if (d.HasMember("valueQuantity") && d["valueQuantity"].IsObject()) {
        const rapidjson::Value& v = d["valueQuantity"];
        if (v.HasMember("value") && v["value"].IsNumber()) {
            const double n = v["value"].GetDouble();
            char buf[64];
            std::snprintf(buf, sizeof(buf), (n == (long long)n) ? "%.0f" : "%g", n);
            value = buf;
        }
        if (v.HasMember("unit") && v["unit"].IsString())      { value += " "; value += v["unit"].GetString(); }
        else if (v.HasMember("code") && v["code"].IsString()) { value += " "; value += v["code"].GetString(); }
    } else if (d.HasMember("valueString") && d["valueString"].IsString()) {
        value = d["valueString"].GetString();
    } else if (d.HasMember("valueCodeableConcept") && d["valueCodeableConcept"].IsObject() &&
               d["valueCodeableConcept"].HasMember("text") && d["valueCodeableConcept"]["text"].IsString()) {
        value = d["valueCodeableConcept"]["text"].GetString();
    }

    std::string when;
    if (d.HasMember("effectiveDateTime") && d["effectiveDateTime"].IsString()) when = d["effectiveDateTime"].GetString();
    else if (d.HasMember("issued") && d["issued"].IsString())                  when = d["issued"].GetString();
    else if (d.HasMember("meta") && d["meta"].IsObject() &&
             d["meta"].HasMember("lastUpdated") && d["meta"]["lastUpdated"].IsString())
        when = d["meta"]["lastUpdated"].GetString();

    out.SetObject();
    out.AddMember("code",  rapidjson::Value(code.c_str(),  (rapidjson::SizeType)code.size(),  a), a);
    out.AddMember("value", rapidjson::Value(value.c_str(), (rapidjson::SizeType)value.size(), a), a);
    out.AddMember("time",  rapidjson::Value(when.c_str(),  (rapidjson::SizeType)when.size(),  a), a);
}

Result family_health(const Args& args, const UserInfo& user, const ToolContext& ctx) {
    if (!user.authed())    return Result::error("Not authenticated");
    if (ctx.db == nullptr) return Result::error("No database available");

    // When the caller gives no `member` but is asking on behalf of a care-circle
    // subject (the "currently for" picker), default to that authorized member so
    // "how is Mom doing?" works without the model restating the id. resolve_subject
    // still re-checks the share, and can_read_health below re-authorizes the read.
    std::string member = args.str("member");
    if (trim(member).empty() && user.subject_user_id > 0) {
        member = std::to_string(user.subject_user_id);
    }
    std::string label;
    const std::int64_t subject = resolve_subject(*ctx.db, user.user_id, member, &label);
    if (subject <= 0) {
        std::vector<circle::HealthShare> people = circle::health_shared_with(*ctx.db, user.user_id);
        std::string msg = "No circle member matching '" + member + "' has shared their health data with you.";
        if (!people.empty()) {
            msg += " Available: ";
            for (std::size_t i = 0; i < people.size(); ++i) { if (i) msg += ", "; msg += display_of(people[i]); }
            msg += ".";
        }
        return Result::error(msg);
    }
    if (subject != user.user_id && !circle::can_read_health(*ctx.db, user.user_id, subject))
        return Result::error("Not authorized to read that member's health data.");

    int count = static_cast<int>(args.integer("count", 20));
    if (count <= 0)   count = 20;
    if (count > 100)  count = 100;

    fhir::FhirStore store(*ctx.db);
    std::int64_t total = 0;
    std::vector<fhir::StoredResource> hits = store.search(subject, "Observation", std::string(), count, 0, total);

    rapidjson::Document d; d.SetObject();
    rapidjson::Document::AllocatorType& a = d.GetAllocator();
    d.AddMember("subject", rapidjson::Value(label.c_str(), (rapidjson::SizeType)label.size(), a), a);
    d.AddMember("observation_count", static_cast<std::int64_t>(total), a);
    rapidjson::Value obs(rapidjson::kArrayType);
    for (std::size_t i = 0; i < hits.size(); ++i) {
        rapidjson::Value o;
        summarize_observation(hits[i].content, o, a);
        if (o.IsObject()) obs.PushBack(o, a);
    }
    d.AddMember("observations", obs, a);
    return Result::ok(to_json(d));
}

const Tool kFamilyHealth = {
    "family_health",
    "Read recent health records (FHIR Observations) for the user or a care-circle "
    "member who has shared their health data. Pass `member` as the person's name, "
    "nickname, email, or numeric id; omit it (or use \"me\") for the user's own "
    "data. Only members who shared their data are reachable. Use this to answer "
    "questions like \"how is Mom doing?\" or \"how did I sleep?\".",
    true,                                                       // auth
    {
        Param("member", Type::String, Optional,
              "Whose health to read: a care-circle member's name / nickname / email / id, "
              "or \"me\" (or omit) for the caller."),
        Param("count", Type::Integer, Optional,
              "How many recent observations to return (default 20, max 100)."),
    },
    &family_health,
};

}   // namespace

MIROBODY_REGISTER_TOOL(kFamilyHealth);
