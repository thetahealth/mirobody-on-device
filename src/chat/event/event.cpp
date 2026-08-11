#include "chat/event/event.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdint>

namespace mirobody { namespace chat {

namespace {

// Serialize a rapidjson value to a compact JSON string.
std::string serialize(const rapidjson::Value& v) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    v.Accept(w);
    return std::string(buf.GetString(), buf.GetSize());
}

// Add a string member to a JSON object value (copying the bytes into `a`).
void add_member(rapidjson::Value& obj, const char* key, const std::string& val,
                rapidjson::Document::AllocatorType& a) {
    obj.AddMember(rapidjson::StringRef(key),
                  rapidjson::Value(val.c_str(), static_cast<rapidjson::SizeType>(val.size()), a), a);
}

// Add a string member to the document root (copying into its allocator).
void add_str(rapidjson::Document& d, const char* key, const std::string& val) {
    add_member(d, key, val, d.GetAllocator());
}

// Build the shared {"type", "content"} object, optionally with "tool_id". The
// caller may add more members before serializing.
rapidjson::Document base_obj(const char* type, const std::string& content,
                             const std::string& tool_id) {
    rapidjson::Document d;
    d.SetObject();
    d.AddMember("type", rapidjson::StringRef(type), d.GetAllocator());
    add_str(d, "content", content);
    if (!tool_id.empty()) add_str(d, "tool_id", tool_id);
    return d;
}

}   // namespace

//------------------------------------------------------------------------------
// to_json
//------------------------------------------------------------------------------

std::string ReplyEvent::to_json() const {
    return serialize(base_obj(type(), content_, std::string()));
}

std::string ThinkingEvent::to_json() const {
    return serialize(base_obj(type(), content_, std::string()));
}

const char* ToolEvent::type() const {
    switch (phase_) {
        case Phase::Title:     return "queryTitle";
        case Phase::Arguments: return "queryArguments";
        case Phase::Detail:    return "queryDetail";
    }
    return "queryTitle";   // unreachable; silences -Wreturn-type
}

std::string ToolEvent::to_json() const {
    return serialize(base_obj(type(), content_, tool_id_));
}

std::string ChartEvent::to_json() const {
    rapidjson::Document d = base_obj(type(), content_, tool_id_);
    rapidjson::Document::AllocatorType& a = d.GetAllocator();

    // Embed the ECharts option as a real nested object so the client can call
    // echarts.setOption(chunk.chart) directly. A parse failure (the option
    // wasn't valid JSON) degrades to an empty object rather than a malformed frame.
    rapidjson::Document opt;
    if (!option_json_.empty() && !opt.Parse(option_json_.c_str()).HasParseError()) {
        d.AddMember("chart", rapidjson::Value(opt, a), a);
    } else {
        d.AddMember("chart", rapidjson::Value(rapidjson::kObjectType), a);
    }
    return serialize(d);
}

// {"type":"ask","content":<question>,"tool_id":<id>,"ask":{...}} — the nested object
// is the choices, mirroring how a chart nests its option.
std::string AskEvent::to_json() const {
    rapidjson::Document d = base_obj(type(), content_, tool_id_);
    rapidjson::Document::AllocatorType& a = d.GetAllocator();

    rapidjson::Document spec;
    if (!spec_json_.empty() && !spec.Parse(spec_json_.c_str()).HasParseError()) {
        d.AddMember("ask", rapidjson::Value(spec, a), a);
    } else {
        d.AddMember("ask", rapidjson::Value(rapidjson::kObjectType), a);
    }
    return serialize(d);
}

std::string AskEvent::payload() const {
    // The same object the SSE form nests, plus the id and question hoisted in, so a
    // flat-transport client parses one thing and has everything it needs to render
    // the question and answer it.
    rapidjson::Document d;
    if (spec_json_.empty() || d.Parse(spec_json_.c_str()).HasParseError() || !d.IsObject()) {
        d.SetObject();
    }
    rapidjson::Document::AllocatorType& a = d.GetAllocator();
    d.RemoveMember("ask_id");
    d.RemoveMember("question");
    d.AddMember("ask_id",
                rapidjson::Value(tool_id_.c_str(),
                                 static_cast<rapidjson::SizeType>(tool_id_.size()), a), a);
    d.AddMember("question",
                rapidjson::Value(content_.c_str(),
                                 static_cast<rapidjson::SizeType>(content_.size()), a), a);
    return serialize(d);
}

std::string CostEvent::to_json() const {
    rapidjson::Document d = base_obj(type(), std::string(), std::string());
    rapidjson::Document::AllocatorType& a = d.GetAllocator();

    rapidjson::Value cost(rapidjson::kObjectType);
    cost.AddMember("model",
                   rapidjson::Value(cost_.model.c_str(),
                                    static_cast<rapidjson::SizeType>(cost_.model.size()), a), a);
    cost.AddMember("input_tokens",  static_cast<std::int64_t>(cost_.input_tokens),  a);
    cost.AddMember("output_tokens", static_cast<std::int64_t>(cost_.output_tokens), a);
    cost.AddMember("total_tokens",  static_cast<std::int64_t>(cost_.total_tokens),  a);
    cost.AddMember("total_cost",    cost_.total_cost, a);
    d.AddMember("cost", cost, a);
    return serialize(d);
}

std::string ErrorEvent::to_json() const {
    return serialize(base_obj(type(), message_, std::string()));
}

std::string UploadEvent::to_json() const {
    rapidjson::Document d = base_obj(type(), filename_, std::string());
    rapidjson::Document::AllocatorType& a = d.GetAllocator();

    // The stored reference as a nested object -- the {filename, mime_type, url,
    // file_key} shape params.files carries and the client already reads.
    rapidjson::Value file(rapidjson::kObjectType);
    add_member(file, "filename",  filename_,  a);
    add_member(file, "mime_type", mime_type_, a);
    add_member(file, "url",       url_,       a);
    add_member(file, "file_key",  file_key_,  a);
    d.AddMember("file", file, a);
    return serialize(d);
}

std::string TranscriptEvent::to_json() const {
    rapidjson::Document d = base_obj(type(), filename_, std::string());
    rapidjson::Document::AllocatorType& a = d.GetAllocator();

    d.AddMember("phase",
                rapidjson::StringRef(phase_ == Phase::Begin ? "begin" : "done"), a);
    if (phase_ == Phase::Done) d.AddMember("extracted", extracted_, a);
    return serialize(d);
}

std::string ConversationEvent::to_json() const {
    rapidjson::Document d = base_obj(type(), std::to_string(id_), std::string());
    d.AddMember("conversation_id", static_cast<std::int64_t>(id_), d.GetAllocator());
    return serialize(d);
}

std::string EndEvent::to_json() const {
    return serialize(base_obj(type(), std::string(), std::string()));
}

//------------------------------------------------------------------------------
// Adapter
//------------------------------------------------------------------------------

std::unique_ptr<Event> event_from_llm(const llm::Event& e) {
    switch (e.type) {
        case llm::EventType::Reply:
            return std::unique_ptr<Event>(new ReplyEvent(e.content));
        case llm::EventType::Thinking:
            return std::unique_ptr<Event>(new ThinkingEvent(e.content));
        case llm::EventType::QueryTitle:
            return std::unique_ptr<Event>(new ToolEvent(ToolEvent::Phase::Title, e.content, e.tool_id));
        case llm::EventType::QueryArguments:
            return std::unique_ptr<Event>(new ToolEvent(ToolEvent::Phase::Arguments, e.content, e.tool_id));
        case llm::EventType::QueryDetail:
            return std::unique_ptr<Event>(new ToolEvent(ToolEvent::Phase::Detail, e.content, e.tool_id));
        case llm::EventType::CostStatistics:
            return std::unique_ptr<Event>(new CostEvent(e.cost));
        case llm::EventType::Error:
            return std::unique_ptr<Event>(new ErrorEvent(e.content));
    }
    return std::unique_ptr<Event>();
}

}}
