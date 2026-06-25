#include "chat/packet.hpp"

namespace mirobody { namespace chat {

// Out-of-line definition for the in-class constant, required pre-C++17 if it is
// ever odr-used (e.g. its address taken / bound to a reference).
const int Packet::kNoCode;

Packet::Packet() : code_(kNoCode) {
    doc_.SetObject();
}

Packet::Packet(int code) : code_(code) {
    doc_.SetObject();
}

Packet::Packet(int code, rapidjson::Document params_object) : code_(code) {
    doc_ = std::move(params_object);
    if (!doc_.IsObject()) doc_.SetObject();
}

Packet Packet::parse(const std::string& text) {
    Packet p;   // code_ = kNoCode, doc_ = empty object
    rapidjson::Document full;
    if (full.Parse(text.data(), text.size()).HasParseError() || !full.IsObject()) {
        return p;
    }

    rapidjson::Value::ConstMemberIterator c = full.FindMember("code");
    if (c != full.MemberEnd()) {
        if (c->value.IsInt())        p.code_ = c->value.GetInt();
        else if (c->value.IsInt64()) p.code_ = static_cast<int>(c->value.GetInt64());
    }

    rapidjson::Value::ConstMemberIterator pit = full.FindMember("params");
    if (pit != full.MemberEnd() && pit->value.IsObject()) {
        p.doc_.CopyFrom(pit->value, p.doc_.GetAllocator());
    }
    return p;
}

//------------------------------------------------------------------------------
// Typed getters -- read directly from the params object (doc_).
//------------------------------------------------------------------------------

bool Packet::has(const char* key) const {
    return doc_.IsObject() && doc_.HasMember(key);
}

std::string Packet::str(const char* key, const std::string& def) const {
    if (!has(key)) return def;
    const rapidjson::Value& m = doc_[key];
    if (m.IsString()) return std::string(m.GetString(), m.GetStringLength());
    return def;
}

long long Packet::integer(const char* key, long long def) const {
    if (!has(key)) return def;
    const rapidjson::Value& m = doc_[key];
    if (m.IsInt64()) return m.GetInt64();
    if (m.IsInt())   return m.GetInt();
    return def;
}

double Packet::number(const char* key, double def) const {
    if (!has(key)) return def;
    const rapidjson::Value& m = doc_[key];
    if (m.IsNumber()) return m.GetDouble();
    return def;
}

bool Packet::boolean(const char* key, bool def) const {
    if (!has(key)) return def;
    const rapidjson::Value& m = doc_[key];
    if (m.IsBool()) return m.GetBool();
    return def;
}

std::vector<std::string> Packet::string_array(const char* key) const {
    std::vector<std::string> out;
    if (!has(key)) return out;
    const rapidjson::Value& m = doc_[key];
    if (!m.IsArray()) return out;
    for (rapidjson::SizeType i = 0; i < m.Size(); ++i) {
        if (m[i].IsString()) {
            out.push_back(std::string(m[i].GetString(), m[i].GetStringLength()));
        }
    }
    return out;
}

}}
