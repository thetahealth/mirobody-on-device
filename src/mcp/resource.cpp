#include "mcp/resource.hpp"

#include "platform/log.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace mirobody { namespace mcp {

namespace {

typedef rapidjson::Document::AllocatorType Alloc;

// Copy a std::string into a rapidjson Value owned by `a` (rapidjson otherwise
// keeps a non-owning pointer for const char*, which would dangle).
rapidjson::Value str_value(const std::string& s, Alloc& a) {
    return rapidjson::Value(s.c_str(), static_cast<rapidjson::SizeType>(s.size()), a);
}

std::string serialize(const rapidjson::Value& v) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    v.Accept(w);
    return std::string(buf.GetString(), buf.GetSize());
}

// Build one MCP resources/list descriptor: { uri, name, description, mimeType }.
// Optional fields are omitted when empty, mirroring the tool descriptor's
// treatment of an absent description.
rapidjson::Value build_descriptor(const Resource& r, Alloc& a) {
    using rapidjson::Value;

    Value res(rapidjson::kObjectType);
    res.AddMember("uri",  str_value(r.uri, a), a);
    res.AddMember("name", str_value(r.name, a), a);
    if (!r.description.empty()) {
        res.AddMember("description", str_value(r.description, a), a);
    }
    if (!r.mime_type.empty()) {
        res.AddMember("mimeType", str_value(r.mime_type, a), a);
    }
    return res;
}

}   // namespace

//------------------------------------------------------------------------------
// Registry
//------------------------------------------------------------------------------

bool ResourceRegistry::add(const Resource& resource) {
    if (resource.uri.empty()) {
        platform::log_warn("mcp: ignoring resource with empty uri");
        return true;
    }
    if (index_.find(resource.uri) != index_.end()) {
        platform::log_warn("mcp: duplicate resource '%s' ignored", resource.uri.c_str());
        return true;
    }
    index_[resource.uri] = resources_.size();
    resources_.push_back(resource);
    platform::log_info("mcp: registered resource '%s'", resource.uri.c_str());
    return true;
}

const Resource* ResourceRegistry::find(const std::string& uri) const {
    std::unordered_map<std::string, std::size_t>::const_iterator it = index_.find(uri);
    if (it == index_.end()) return nullptr;
    return &resources_[it->second];
}

std::vector<std::string> ResourceRegistry::uris() const {
    std::vector<std::string> out;
    out.reserve(resources_.size());
    for (std::size_t i = 0; i < resources_.size(); ++i) out.push_back(resources_[i].uri);
    return out;
}

std::string ResourceRegistry::resources_list_json() const {
    rapidjson::Document d;
    d.SetArray();
    Alloc& a = d.GetAllocator();
    for (std::size_t i = 0; i < resources_.size(); ++i) {
        d.PushBack(build_descriptor(resources_[i], a), a);
    }
    return serialize(d);
}

ResourceResult ResourceRegistry::read(const std::string& uri, const UserInfo& user) const {
    const Resource* r = find(uri);
    if (!r) {
        return ResourceResult::error("Unknown resource: " + uri);
    }
    if (!r->handler) {
        return ResourceResult::error("Resource has no handler: " + uri);
    }
    try {
        return r->handler(user);
    } catch (const std::exception& e) {
        platform::log_error("mcp resource '%s' threw: %s", uri.c_str(), e.what());
        return ResourceResult::error(e.what());
    } catch (...) {
        platform::log_error("mcp resource '%s' threw a non-std exception", uri.c_str());
        return ResourceResult::error("Unknown error in resource: " + uri);
    }
}

//------------------------------------------------------------------------------

ResourceRegistry& resource_registry() {
    static ResourceRegistry instance;
    return instance;
}

}
}
