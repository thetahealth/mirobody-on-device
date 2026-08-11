// Implementation of the pure C-ABI transforms declared in c_api_json.hpp.
// See that header for why these live apart from c_api.cpp.

#include "platform/c_api_json.hpp"

namespace mirobody { namespace platform {

namespace {

// The first string member of `v` named `name`, else "".
std::string json_str(const rapidjson::Value& v, const char* name) {
    rapidjson::Value::ConstMemberIterator it = v.FindMember(name);
    if (it == v.MemberEnd() || !it->value.IsString()) return std::string();
    return std::string(it->value.GetString(), it->value.GetStringLength());
}

// `parent.<member>.coding[0]`, the CodeableConcept shape both `code` and
// `method` use, or null when any link in that chain is missing or the wrong
// type.
const rapidjson::Value* first_coding(const rapidjson::Value& parent, const char* member) {
    rapidjson::Value::ConstMemberIterator cc = parent.FindMember(member);
    if (cc == parent.MemberEnd() || !cc->value.IsObject()) return nullptr;
    rapidjson::Value::ConstMemberIterator coding = cc->value.FindMember("coding");
    if (coding == cc->value.MemberEnd() || !coding->value.IsArray() ||
        coding->value.Size() == 0 || !coding->value[0].IsObject()) {
        return nullptr;
    }
    return &coding->value[0];
}

}   // namespace

//------------------------------------------------------------------------------

ProviderToken split_provider(const std::string& pair) {
    ProviderToken out;
    const std::string::size_type slash = pair.find('/');
    if (slash == std::string::npos) {
        out.agent = pair;
    } else {
        out.agent = pair.substr(0, slash);
        out.model = pair.substr(slash + 1);
    }
    return out;
}

//------------------------------------------------------------------------------

ChatInput parse_chat_messages(const char* messages_json) {
    ChatInput out;

    rapidjson::Document d;
    if (!messages_json || d.Parse(messages_json).HasParseError() || !d.IsArray()) {
        out.error = ChatInputError::NotAnArray;
        return out;
    }

    for (rapidjson::SizeType i = 0; i < d.Size(); ++i) {
        const rapidjson::Value& m = d[i];
        if (!m.IsObject()) continue;
        rapidjson::Value::ConstMemberIterator role    = m.FindMember("role");
        rapidjson::Value::ConstMemberIterator content = m.FindMember("content");
        if (role == m.MemberEnd()    || !role->value.IsString())    continue;
        if (content == m.MemberEnd() || !content->value.IsString()) continue;

        llm::ChatMessage msg;
        msg.role.assign(role->value.GetString(), role->value.GetStringLength());
        msg.content.assign(content->value.GetString(), content->value.GetStringLength());
        // Overwritten by each later user turn, so the last one wins.
        if (msg.role == "user") out.question = msg.content;
        out.messages.push_back(msg);
    }

    if (out.messages.empty()) out.error = ChatInputError::NoUsableMessages;
    return out;
}

//------------------------------------------------------------------------------

HealthItem flatten_observation(const rapidjson::Value& resource) {
    HealthItem item;
    if (!resource.IsObject()) return item;

    // code.coding[0] -- the LOINC code + display written on ingest.
    if (const rapidjson::Value* coding = first_coding(resource, "code")) {
        item.code    = json_str(*coding, "code");
        item.display = json_str(*coding, "display");
    }

    // valueQuantity, or -- for a panel such as blood pressure, where the
    // Observation itself carries no value -- the first component's.
    const rapidjson::Value* vq = nullptr;
    rapidjson::Value::ConstMemberIterator direct = resource.FindMember("valueQuantity");
    if (direct != resource.MemberEnd() && direct->value.IsObject()) {
        vq = &direct->value;
    } else {
        rapidjson::Value::ConstMemberIterator comp = resource.FindMember("component");
        if (comp != resource.MemberEnd() && comp->value.IsArray() &&
            comp->value.Size() > 0 && comp->value[0].IsObject()) {
            rapidjson::Value::ConstMemberIterator cvq =
                comp->value[0].FindMember("valueQuantity");
            if (cvq != comp->value[0].MemberEnd() && cvq->value.IsObject()) vq = &cvq->value;
        }
    }
    if (vq) {
        rapidjson::Value::ConstMemberIterator v = vq->FindMember("value");
        if (v != vq->MemberEnd() && v->value.IsNumber()) {
            item.value     = v->value.GetDouble();
            item.has_value = true;
        }
        // UCUM `code` is the machine-readable unit; `unit` is the human label
        // and only stands in when no code was recorded.
        item.unit = json_str(*vq, "code");
        if (item.unit.empty()) item.unit = json_str(*vq, "unit");
    }

    // An instant sample carries effectiveDateTime; an interval one (a day's
    // steps, a night's sleep) carries effectivePeriod.start.
    item.when = json_str(resource, "effectiveDateTime");
    if (item.when.empty()) {
        rapidjson::Value::ConstMemberIterator p = resource.FindMember("effectivePeriod");
        if (p != resource.MemberEnd() && p->value.IsObject()) {
            item.when = json_str(p->value, "start");
        }
    }

    // method.coding[0].display -- the ingesting source, set on ingest.
    if (const rapidjson::Value* method = first_coding(resource, "method")) {
        item.source = json_str(*method, "display");
    }

    return item;
}

bool flatten_observation_json(const char* json, std::size_t len, HealthItem& out) {
    if (!json) return false;
    rapidjson::Document res;
    if (res.Parse(json, len).HasParseError() || !res.IsObject()) return false;
    out = flatten_observation(res);
    return true;
}

bool later_reading(const HealthItem& a, const HealthItem& b) {
    if (a.when.empty() != b.when.empty()) return !a.when.empty();
    return a.when > b.when;
}

}
}
