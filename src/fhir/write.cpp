#include "fhir/write.hpp"

#include "platform/clock.hpp"   // now_unix_ms

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdio>
#include <ctime>
#include <random>

namespace mirobody { namespace fhir {

namespace {

using rapidjson::Document;
using rapidjson::Value;

std::string serialize(const Value& v) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    v.Accept(w);
    return std::string(buf.GetString(), buf.GetSize());
}

}  // namespace

std::string new_resource_id() {
    // std::random is fine here (ordinary server code); seeded once.
    static std::mt19937_64 rng((std::random_device()()));
    std::uniform_int_distribution<std::uint64_t> dist;
    std::uint64_t a = dist(rng), b = dist(rng);
    a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;  // version 4
    b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;  // variant 1
    char s[37];
    std::snprintf(s, sizeof(s), "%08x-%04x-%04x-%04x-%012llx",
                  static_cast<unsigned>(a >> 32),
                  static_cast<unsigned>((a >> 16) & 0xFFFF),
                  static_cast<unsigned>(a & 0xFFFF),
                  static_cast<unsigned>(b >> 48),
                  static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFULL));
    return std::string(s);
}

std::string iso8601_instant(std::int64_t unix_ms) {
    std::time_t t = static_cast<std::time_t>(unix_ms / 1000);
    std::tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return std::string(buf);
}

void inject_meta(Document& resource, const std::string& id,
                 std::int64_t version, const std::string& last_updated) {
    Document::AllocatorType& a = resource.GetAllocator();

    resource.RemoveMember("id");
    resource.AddMember("id", Value(id.c_str(), a), a);

    Value* meta;
    Value::MemberIterator it = resource.FindMember("meta");
    if (it != resource.MemberEnd() && it->value.IsObject()) {
        meta = &it->value;
    } else {
        resource.RemoveMember("meta");
        resource.AddMember("meta", Value(rapidjson::kObjectType), a);
        meta = &resource["meta"];
    }
    meta->RemoveMember("versionId");
    meta->AddMember("versionId", Value(std::to_string(version).c_str(), a), a);
    meta->RemoveMember("lastUpdated");
    meta->AddMember("lastUpdated", Value(last_updated.c_str(), a), a);
}

WriteResult write_resource(FhirStore& store, std::int64_t user_id,
                           const std::string& type, const std::string& id,
                           Document& resource) {
    WriteResult out;
    out.id = id.empty() ? new_resource_id() : id;

    // A fresh id cannot collide (uuid), so the lookup is only meaningful for a
    // caller-chosen one. Version counts up across a soft delete: the deleted row
    // is a version of this resource's history, not a reason to restart at 1.
    if (!id.empty()) {
        StoredResource prior;
        if (store.get(user_id, type, out.id, prior)) {
            out.existed = !prior.deleted;
            out.version = prior.version_id + 1;
        }
    }
    out.updated_at = platform::now_unix_ms();

    inject_meta(resource, out.id, out.version, iso8601_instant(out.updated_at));
    out.content = serialize(resource);

    StoredResource sr;
    sr.type       = type;
    sr.id         = out.id;
    sr.version_id = out.version;
    sr.updated_at = out.updated_at;
    sr.deleted    = false;
    sr.content    = out.content;
    store.upsert(user_id, sr);

    return out;
}

}}  // namespace mirobody::fhir
