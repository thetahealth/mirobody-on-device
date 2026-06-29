#include "chat/params.hpp"

#include "chat/packet.hpp"
#include "llm/mirothinker.hpp"   // llm::ChatMessage
#include "platform/log.hpp"

#include <rapidjson/document.h>

#include <cstdlib>   // strtoll

namespace mirobody { namespace chat {

namespace {

// Read a string member from a JSON object, or "" if absent / not a string.
std::string member_str(const rapidjson::Value& obj, const char* key) {
    if (!obj.IsObject()) return std::string();
    rapidjson::Value::ConstMemberIterator it = obj.FindMember(key);
    if (it != obj.MemberEnd() && it->value.IsString()) {
        return std::string(it->value.GetString(), it->value.GetStringLength());
    }
    return std::string();
}

// Append {role, content} objects from a rapidjson array onto `out`, skipping
// entries without a role. Shared by the JSON body and a form's "messages" field
// (the same array as a JSON-encoded string).
void append_messages(const rapidjson::Value& arr, std::vector<llm::ChatMessage>& out) {
    for (rapidjson::SizeType i = 0; i < arr.Size(); ++i) {
        if (!arr[i].IsObject()) continue;
        llm::ChatMessage m;
        if (arr[i].HasMember("role") && arr[i]["role"].IsString()) {
            m.role.assign(arr[i]["role"].GetString(), arr[i]["role"].GetStringLength());
        }
        if (arr[i].HasMember("content") && arr[i]["content"].IsString()) {
            m.content.assign(arr[i]["content"].GetString(), arr[i]["content"].GetStringLength());
        }
        if (!m.role.empty()) out.push_back(m);
    }
}

}   // namespace

ChatParams ChatParams::parse(const Packet& pkt, std::int64_t user_id) {
    ChatParams cp;
    cp.agent = pkt.str("agent");

    AgentRequest& areq = cp.request;
    areq.user_id    = user_id;
    // session_id / timezone were stamped into the packet by the transport (the
    // X-Session-Id / X-Timezone headers winning over any body field).
    areq.session_id = pkt.str("session_id");
    areq.provider   = pkt.str("provider");
    areq.language   = pkt.str("language");
    // Stamped from the Accept-Language header by the transport (see sse.cpp).
    areq.accept_language = pkt.str("accept_language");
    areq.timezone   = pkt.str("timezone");
    areq.question   = pkt.str("question");

    const rapidjson::Value& params = pkt.params();

    // conversation_id: continue an existing server-side thread (absent / 0 / an
    // id the caller does not own => a new thread is started). Accept a JSON
    // number or a decimal string -- the client carries large ids as strings to
    // avoid JS losing integer precision past 2^53.
    rapidjson::Value::ConstMemberIterator cit = params.FindMember("conversation_id");
    if (cit != params.MemberEnd()) {
        if (cit->value.IsInt64() || cit->value.IsInt()) {
            areq.conversation_id = cit->value.GetInt64();
        } else if (cit->value.IsString()) {
            areq.conversation_id = std::strtoll(cit->value.GetString(), nullptr, 10);
        }
    }

    // subject: the "currently for" care-circle member whose health the caller
    // wants to ask about, as an opaque member handle (care_circle_members.id).
    // Stored here as-is and resolved to the real target user id (with an access
    // check) in the dispatcher. Number or decimal string. `subject_user_id` is
    // still accepted as a legacy alias for the same handle field.
    rapidjson::Value::ConstMemberIterator sit = params.FindMember("subject");
    if (sit == params.MemberEnd()) sit = params.FindMember("subject_user_id");
    if (sit != params.MemberEnd()) {
        if (sit->value.IsInt64() || sit->value.IsInt()) {
            areq.subject_user_id = sit->value.GetInt64();
        } else if (sit->value.IsString()) {
            areq.subject_user_id = std::strtoll(sit->value.GetString(), nullptr, 10);
        }
    }

    rapidjson::Value::ConstMemberIterator mit = params.FindMember("messages");
    if (mit != params.MemberEnd() && mit->value.IsArray()) {
        append_messages(mit->value, areq.messages);
    }

    // Files were stored by the dispatcher's upload preprocess and referenced
    // here by URL/key -- AgentRequest carries no inline bytes.
    rapidjson::Value::ConstMemberIterator fit = params.FindMember("files");
    if (fit != params.MemberEnd() && fit->value.IsArray()) {
        const rapidjson::Value& arr = fit->value;
        for (rapidjson::SizeType i = 0; i < arr.Size(); ++i) {
            if (!arr[i].IsObject()) continue;
            AgentFile af;
            af.filename  = member_str(arr[i], "filename");
            af.mime_type = member_str(arr[i], "mime_type");
            af.url       = member_str(arr[i], "url");
            af.file_key  = member_str(arr[i], "file_key");
            platform::log_debug("chat[3/params]: file '%s' (mime=%s, url=%s, key=%s)",
                                af.filename.c_str(), af.mime_type.c_str(),
                                af.url.c_str(), af.file_key.c_str());
            areq.files.push_back(af);
        }
        platform::log_debug("chat[3/params]: parsed %lu file(s) into request",
                            (unsigned long)areq.files.size());
    }

    // Overlay the raw upload bytes (still held on the packet's attachments after
    // the dispatcher stored them) onto the matching file refs by filename, so an
    // agent can inline file content (e.g. Gemini inlineData) instead of passing a
    // URL the model can't fetch. Reference-only files (no attachment) keep empty
    // data and fall back to the URL.
    const std::vector<Attachment>& atts = pkt.attachments();
    for (std::size_t k = 0; k < atts.size(); ++k) {
        for (std::size_t j = 0; j < areq.files.size(); ++j) {
            if (areq.files[j].data.empty() &&
                areq.files[j].filename == atts[k].filename) {
                areq.files[j].data = atts[k].data;
                break;
            }
        }
    }

    // "question" with no explicit messages becomes the single user turn.
    if (areq.messages.empty() && !areq.question.empty()) {
        llm::ChatMessage m;
        m.role    = "user";
        m.content = areq.question;
        areq.messages.push_back(m);
    }

    return cp;
}

LiveParams LiveParams::parse(const Packet& pkt) {
    LiveParams lp;
    lp.request.provider = pkt.str("provider");
    lp.request.system   = pkt.str("system");
    if (lp.request.system.empty()) lp.request.system = pkt.str("system_prompt");

    const rapidjson::Value& params = pkt.params();
    rapidjson::Value::ConstMemberIterator mit = params.FindMember("messages");
    if (mit != params.MemberEnd() && mit->value.IsArray()) {
        append_messages(mit->value, lp.request.messages);
    }
    if (lp.request.messages.empty()) {
        const std::string q = pkt.str("question");
        if (!q.empty()) {
            llm::ChatMessage m; m.role = "user"; m.content = q;
            lp.request.messages.push_back(m);
        }
    }
    return lp;
}

}}
