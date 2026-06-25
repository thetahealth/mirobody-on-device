#include "mcp/tool.hpp"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace mirobody { namespace mcp {

namespace {

typedef rapidjson::Document::AllocatorType Alloc;

//------------------------------------------------------------------------------

const char* type_name(Type t) {
    switch (t) {
        case Type::String:  return "string";
        case Type::Integer: return "integer";
        case Type::Number:  return "number";
        case Type::Boolean: return "boolean";
        case Type::Array:   return "array";
        case Type::Object:  return "object";
    }
    return "string";
}

// Copy a std::string into a rapidjson Value owned by `a` (rapidjson otherwise
// keeps a non-owning pointer for const char*, which would dangle).
rapidjson::Value str_value(const std::string& s, Alloc& a) {
    return rapidjson::Value(s.c_str(), static_cast<rapidjson::SizeType>(s.size()), a);
}

//------------------------------------------------------------------------------
// Build the MCP inputSchema for one tool.

rapidjson::Value build_input_schema(const Tool& t, Alloc& a) {
    using rapidjson::Value;

    // Escape hatch: a verbatim schema overrides the generated one.
    if (!t.raw_input_schema.empty()) {
        rapidjson::Document raw;
        if (!raw.Parse(t.raw_input_schema.c_str()).HasParseError()) {
            return Value(raw, a);   // deep-copy into the target allocator
        }
        platform::log_warn("mcp tool '%s' has an invalid raw_input_schema; "
                           "falling back to generated schema", t.name.c_str());
    }

    Value schema(rapidjson::kObjectType);
    schema.AddMember("type", "object", a);

    Value properties(rapidjson::kObjectType);
    Value required(rapidjson::kArrayType);

    for (std::size_t i = 0; i < t.params.size(); ++i) {
        const Param& p = t.params[i];

        Value prop(rapidjson::kObjectType);
        prop.AddMember("type", Value(type_name(p.type), a), a);

        if (p.type == Type::Array) {
            Value items(rapidjson::kObjectType);
            items.AddMember("type", Value(type_name(p.item), a), a);
            prop.AddMember("items", items, a);
        }

        if (!p.description.empty()) {
            prop.AddMember("description", str_value(p.description, a), a);
        }

        properties.AddMember(str_value(p.name, a), prop, a);

        if (p.required) {
            required.PushBack(str_value(p.name, a), a);
        }
    }

    schema.AddMember("properties", properties, a);

    // Omit an empty `required` array -- some APIs (e.g. Gemini) reject it,
    // matching the cleanup at the tail of Python's parse_function.
    if (!required.Empty()) {
        schema.AddMember("required", required, a);
    }

    return schema;
}

// Build one MCP tools/list descriptor.
rapidjson::Value build_descriptor(const Tool& t, Alloc& a) {
    using rapidjson::Value;

    Value tool(rapidjson::kObjectType);
    tool.AddMember("name",        str_value(t.name, a), a);
    tool.AddMember("description", str_value(t.description, a), a);
    tool.AddMember("inputSchema", build_input_schema(t, a), a);

    Value annotations(rapidjson::kObjectType);
    annotations.AddMember("title",           str_value(t.name, a), a);
    annotations.AddMember("destructiveHint", false, a);
    annotations.AddMember("openWorldHint",   false, a);
    annotations.AddMember("readOnlyHint",    true,  a);
    tool.AddMember("annotations", annotations, a);

    return tool;
}

std::string serialize(const rapidjson::Value& v) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    v.Accept(w);
    return std::string(buf.GetString(), buf.GetSize());
}

}   // namespace

//------------------------------------------------------------------------------
// Args
//------------------------------------------------------------------------------

bool Args::has(const char* key) const {
    return v_ && v_->IsObject() && v_->HasMember(key);
}

std::string Args::str(const char* key, const std::string& def) const {
    if (!has(key)) return def;
    const rapidjson::Value& m = (*v_)[key];
    if (m.IsString()) return std::string(m.GetString(), m.GetStringLength());
    return def;
}

long long Args::integer(const char* key, long long def) const {
    if (!has(key)) return def;
    const rapidjson::Value& m = (*v_)[key];
    if (m.IsInt64()) return m.GetInt64();
    if (m.IsInt())   return m.GetInt();
    return def;
}

double Args::number(const char* key, double def) const {
    if (!has(key)) return def;
    const rapidjson::Value& m = (*v_)[key];
    if (m.IsNumber()) return m.GetDouble();
    return def;
}

bool Args::boolean(const char* key, bool def) const {
    if (!has(key)) return def;
    const rapidjson::Value& m = (*v_)[key];
    if (m.IsBool()) return m.GetBool();
    return def;
}

std::vector<std::string> Args::string_array(const char* key) const {
    std::vector<std::string> out;
    if (!has(key)) return out;
    const rapidjson::Value& m = (*v_)[key];
    if (!m.IsArray()) return out;
    for (rapidjson::SizeType i = 0; i < m.Size(); ++i) {
        if (m[i].IsString()) {
            out.push_back(std::string(m[i].GetString(), m[i].GetStringLength()));
        }
    }
    return out;
}

//------------------------------------------------------------------------------
// Registry
//------------------------------------------------------------------------------

bool Registry::add(const Tool& tool) {
    if (tool.name.empty()) {
        platform::log_warn("mcp: ignoring tool with empty name");
        return true;
    }
    if (index_.find(tool.name) != index_.end()) {
        platform::log_warn("mcp: duplicate tool '%s' ignored", tool.name.c_str());
        return true;
    }
    index_[tool.name] = tools_.size();
    tools_.push_back(tool);
    platform::log_info("mcp: registered tool '%s'", tool.name.c_str());
    return true;
}

const Tool* Registry::find(const std::string& name) const {
    std::unordered_map<std::string, std::size_t>::const_iterator it = index_.find(name);
    if (it == index_.end()) return nullptr;
    return &tools_[it->second];
}

std::vector<std::string> Registry::names() const {
    std::vector<std::string> out;
    out.reserve(tools_.size());
    for (std::size_t i = 0; i < tools_.size(); ++i) out.push_back(tools_[i].name);
    return out;
}

std::string Registry::tools_list_json() const {
    rapidjson::Document d;
    d.SetArray();
    Alloc& a = d.GetAllocator();
    for (std::size_t i = 0; i < tools_.size(); ++i) {
        d.PushBack(build_descriptor(tools_[i], a), a);
    }
    return serialize(d);
}

std::string Registry::functions_json(const char* style) const {
    using rapidjson::Value;

    const bool gemini = style && std::string(style) == "gemini";
    const bool openai = style && std::string(style) == "openai";

    rapidjson::Document d;
    d.SetArray();
    Alloc& a = d.GetAllocator();

    for (std::size_t i = 0; i < tools_.size(); ++i) {
        const Tool& t = tools_[i];

        Value name        = str_value(t.name, a);
        Value description  = str_value(t.description, a);
        Value parameters   = build_input_schema(t, a);

        if (gemini) {
            // { "name", "description", "parameters" }
            Value fn(rapidjson::kObjectType);
            fn.AddMember("name", name, a);
            fn.AddMember("description", description, a);
            fn.AddMember("parameters", parameters, a);
            d.PushBack(fn, a);

        } else if (openai) {
            // { "type": "function", "function": { name, description, parameters } }
            Value inner(rapidjson::kObjectType);
            inner.AddMember("name", name, a);
            inner.AddMember("description", description, a);
            inner.AddMember("parameters", parameters, a);

            Value fn(rapidjson::kObjectType);
            fn.AddMember("type", "function", a);
            fn.AddMember("function", inner, a);
            d.PushBack(fn, a);

        } else {
            // OpenAI simplified (Responses): { "type", "name", ... } flattened.
            Value fn(rapidjson::kObjectType);
            fn.AddMember("type", "function", a);
            fn.AddMember("name", name, a);
            fn.AddMember("description", description, a);
            fn.AddMember("parameters", parameters, a);
            d.PushBack(fn, a);
        }
    }

    return serialize(d);
}

Result Registry::call(const std::string&      name,
                      const rapidjson::Value& arguments,
                      const UserInfo&         user,
                      const ToolContext&      ctx) const {
    const Tool* t = find(name);
    if (!t) {
        return Result::error("Unsupported tool: " + name);
    }
    if (!t->handler) {
        return Result::error("Tool has no handler: " + name);
    }

    Args args(arguments);
    try {
        return t->handler(args, user, ctx);
    } catch (const std::exception& e) {
        platform::log_error("mcp tool '%s' threw: %s", name.c_str(), e.what());
        return Result::error(e.what());
    } catch (...) {
        platform::log_error("mcp tool '%s' threw a non-std exception", name.c_str());
        return Result::error("Unknown error in tool: " + name);
    }
}

//------------------------------------------------------------------------------
// Free functions
//------------------------------------------------------------------------------

Registry& registry() {
    static Registry instance;
    return instance;
}

std::string to_json(const rapidjson::Value& v) {
    return serialize(v);
}

std::string run_mcp_tool(const std::string& name,
                         const std::string& args_json,
                         const UserInfo&    user,
                         const ToolContext& ctx) {
    rapidjson::Document d;
    if (d.Parse(args_json.c_str()).HasParseError() || !d.IsObject()) {
        d.SetObject();   // tolerate empty / malformed args -> tool sees defaults
    }
    const Result r = registry().call(name, d, user, ctx);
    return r.success ? r.json : (std::string{"Error: "} + r.json);
}

}
}
