#include "config/store.hpp"

#include "client/http_client.hpp"
#include "platform/log.hpp"

#include <rapidjson/document.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

namespace mirobody { namespace utils {

namespace {

//------------------------------------------------------------------------------
// Helpers
//------------------------------------------------------------------------------

// Matches Python's FernetEncrypter.is_encrypted: any value beginning with
// the canonical Fernet token prefix is treated as ciphertext to decrypt.
const std::string kEncryptedPrefix = "gAAAA";

//------------------------------------------------------------------------------

std::string to_upper(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return out;
}

//------------------------------------------------------------------------------

std::string trim(const std::string& s) {
    auto is_ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    std::size_t b = 0, e = s.size();
    while (b < e && is_ws(s[b]))     ++b;
    while (e > b && is_ws(s[e - 1])) --e;
    return s.substr(b, e - b);
}

//------------------------------------------------------------------------------

bool looks_encrypted(const std::string& s) {
    return s.size() >= kEncryptedPrefix.size() && s.compare(0, kEncryptedPrefix.size(), kEncryptedPrefix) == 0;
}

//------------------------------------------------------------------------------

mirobody::optional<std::string> getenv_opt(const std::string& key) {
    const char* v = std::getenv(key.c_str());
    if (!v || *v == '\0') return mirobody::nullopt;
    return std::string{v};
}

//------------------------------------------------------------------------------

// URL-safe base64 encode. Local copy to keep config decoupled from the
// fernet translation unit; the encoder is small and the two never share
// state. If a third caller needs it, hoist it into a utils/base64 module.
std::string b64url_encode(const std::uint8_t* data, std::size_t len) {
    constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789-_";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (std::size_t i = 0; i < len; i += 3) {
        std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
        std::size_t take = 1;
        if (i + 1 < len) { n |= static_cast<std::uint32_t>(data[i + 1]) << 8; take = 2; }
        if (i + 2 < len) { n |= static_cast<std::uint32_t>(data[i + 2]);      take = 3; }
        out.push_back(alphabet[(n >> 18) & 0x3F]);
        out.push_back(alphabet[(n >> 12) & 0x3F]);
        out.push_back(take >= 2 ? alphabet[(n >> 6) & 0x3F] : '=');
        out.push_back(take >= 3 ? alphabet[n & 0x3F]        : '=');
    }
    return out;
}

//------------------------------------------------------------------------------

std::string scalar_to_string(const rapidjson::Value& v) {
    if (v.IsString()) return v.GetString();
    if (v.IsBool())   return v.GetBool() ? "true" : "false";
    if (v.IsInt64())  return std::to_string(v.GetInt64());
    if (v.IsUint64()) return std::to_string(v.GetUint64());
    if (v.IsDouble()) return std::to_string(v.GetDouble());
    return {};
}

//------------------------------------------------------------------------------

mirobody::optional<std::unordered_map<std::string, std::string>> try_parse_dict_json(const std::string& s) {
    rapidjson::Document doc;
    if (doc.Parse(s.c_str()).HasParseError() || !doc.IsObject()) return mirobody::nullopt;
    std::unordered_map<std::string, std::string> out;
    for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
        auto v = scalar_to_string(it->value);
        if (v.empty() && !it->value.IsString()) continue;
        out.emplace(it->name.GetString(), std::move(v));
    }
    return out;
}

//------------------------------------------------------------------------------

mirobody::optional<std::vector<std::string>> try_parse_list_json(const std::string& s) {
    rapidjson::Document doc;
    if (doc.Parse(s.c_str()).HasParseError() || !doc.IsArray()) return mirobody::nullopt;
    std::vector<std::string> out;
    out.reserve(doc.Size());
    for (const auto& v : doc.GetArray()) {
        auto sv = scalar_to_string(v);
        if (sv.empty() && !v.IsString()) continue;
        out.push_back(std::move(sv));
    }
    return out;
}

//------------------------------------------------------------------------------

// Mirrors Python's regex `_KEY|_PASSWORD|_PASS|_PWD|_SECRET|_SK|_TOKEN`,
// excluding keys that end in `_URL` (which are public endpoints even when
// they syntactically contain one of those substrings).
bool is_sensitive(const std::string& upper_key) {
    const std::string url_suffix = "_URL";
    if (upper_key.size() >= url_suffix.size() &&
        upper_key.compare(upper_key.size() - url_suffix.size(), url_suffix.size(), url_suffix) == 0) {
        return false;
    }
    const char* const patterns[] = {
        "_KEY", "_PASSWORD", "_PASS", "_PWD", "_SECRET", "_SK", "_TOKEN",
    };
    for (const char* p : patterns) {
        if (upper_key.find(p) != std::string::npos) return true;
    }
    return false;
}

//------------------------------------------------------------------------------

std::string mask_value(const std::string& s) {
    if (s.empty())    return "<not set>";
    if (s.size() < 6) return "************";
    return s.substr(0, 3) + "******" + s.substr(s.size() - 3);
}

//------------------------------------------------------------------------------

std::string format_node(const YAML::Node& n) {
    if (n.IsScalar())   return n.Scalar();
    if (n.IsSequence()) return "<list of " + std::to_string(n.size()) + " items>";
    if (n.IsMap())      return "<map of "  + std::to_string(n.size()) + " entries>";
    if (n.IsNull())     return "<null>";
    return "<unknown>";
}

}

//------------------------------------------------------------------------------
// ConfigStore
//------------------------------------------------------------------------------

ConfigStore::ConfigStore(std::shared_ptr<encrypt::Fernet> encrypter) : encrypter_(std::move(encrypter)) {}

//------------------------------------------------------------------------------

void ConfigStore::set_encrypter(std::shared_ptr<encrypt::Fernet> encrypter) {
    encrypter_ = std::move(encrypter);
}

//------------------------------------------------------------------------------

std::string ConfigStore::derive_fernet_key(const std::string& secret) {
    std::string trimmed = trim(secret);
    if (trimmed.size() > 32) trimmed = trimmed.substr(0, 32);

    std::array<std::uint8_t, 32> buf{};
    std::size_t i = 0;
    for (; i < trimmed.size(); ++i) buf[i] = static_cast<std::uint8_t>(trimmed[i]);
    // Right-pad with ASCII '0' (0x30), matching Python's b.ljust(32, b"0").
    for (; i < buf.size(); ++i)     buf[i] = static_cast<std::uint8_t>('0');
    return b64url_encode(buf.data(), buf.size());
}

//------------------------------------------------------------------------------

bool ConfigStore::load_yaml_string(const std::string& content) {
    if (content.empty()) return false;
    try {
        YAML::Node root = YAML::Load(std::string{content});
        ingest_yaml_root(root);
        return true;
    } catch (const YAML::Exception& e) {
        platform::log_warn("config: YAML parse error: %s", e.what());
        return false;
    }
}

//------------------------------------------------------------------------------

void ConfigStore::absorb(const ConfigStore& other) {
    for (const auto& kv : other.raw_) raw_[kv.first] = kv.second;
}

//------------------------------------------------------------------------------

void ConfigStore::ingest_yaml_root(const YAML::Node& root) {
    if (!root || !root.IsMap()) return;

    for (const auto& kv : root) {
        if (!kv.first.IsScalar()) continue;
        std::string key = to_upper(kv.first.Scalar());
        YAML::Node value = kv.second;
        if (encrypter_ && value.IsScalar() && looks_encrypted(value.Scalar())) {
            try {
                std::string decrypted = encrypter_->decrypt(value.Scalar());
                YAML::Node n;
                n = decrypted;
                raw_[key] = n;
                continue;
            } catch (const encrypt::FernetError& e) {
                platform::log_warn("config: failed to decrypt %s: %s", key.c_str(), e.what());
            }
        }
        raw_[key] = value;
    }
}

//------------------------------------------------------------------------------

const YAML::Node* ConfigStore::lookup(const std::string& upper_key) const {
    auto it = raw_.find(upper_key);
    return it == raw_.end() ? nullptr : &it->second;
}

//------------------------------------------------------------------------------

bool ConfigStore::has(const std::string& key) const {
    std::string k{trim(key)};
    if (k.empty()) return false;
    if (getenv_opt(k)) return true;
    std::string upper = to_upper(k);
    if (upper != k && getenv_opt(upper)) return true;
    return raw_.find(upper) != raw_.end();
}

//------------------------------------------------------------------------------

std::string ConfigStore::get_str(const std::string& key, const std::string& default_value) const {
    std::string k{trim(key)};
    if (k.empty()) return std::string{default_value};

    if (auto v = getenv_opt(k)) return *v;
    std::string upper = to_upper(k);
    if (upper != k) {
        if (auto v = getenv_opt(upper)) return *v;
    }
    {
        // An empty scalar counts as unset, like an empty env var (getenv_opt):
        // 'KEY: ""' falls through to the default rather than overriding it.
        const YAML::Node* node = lookup(upper);
        if (node && node->IsScalar() && !node->Scalar().empty()) return node->Scalar();
    }
    return std::string{default_value};
}

//------------------------------------------------------------------------------

std::int64_t ConfigStore::get_int(const std::string& key, std::int64_t default_value) const {
    std::string s = get_str(key);
    if (s.empty()) return default_value;
    try {
        std::size_t pos = 0;
        long long v = std::stoll(s, &pos);
        if (pos == s.size()) return static_cast<std::int64_t>(v);
    } catch (...) {}
    return default_value;
}

//------------------------------------------------------------------------------

bool ConfigStore::get_bool(const std::string& key, bool default_value) const {
    std::string s = get_str(key);
    if (s.empty()) return default_value;

    std::string upper = to_upper(s);
    std::string trimmed = trim(upper);
    if (trimmed == "TRUE")  return true;
    if (trimmed == "FALSE") return false;

    try {
        std::size_t pos = 0;
        long long v = std::stoll(std::string{trimmed}, &pos);
        if (pos == trimmed.size()) return v != 0;
    } catch (...) {}
    return default_value;
}

//------------------------------------------------------------------------------

std::unordered_map<std::string, std::string>
ConfigStore::get_dict(const std::string& key, std::unordered_map<std::string, std::string> default_value) const {
    std::string k{trim(key)};
    if (k.empty()) return default_value;
    std::string upper = to_upper(k);

    if (auto v = getenv_opt(k)) {
        if (auto m = try_parse_dict_json(*v)) return *m;
    }
    if (upper != k) {
        if (auto v = getenv_opt(upper)) {
            if (auto m = try_parse_dict_json(*v)) return *m;
        }
    }

    auto* node = lookup(upper);

    if (node) {
        if (node->IsMap()) {
            std::unordered_map<std::string, std::string> out;
            for (const auto& kv : *node) {
                if (!kv.first.IsScalar() || !kv.second.IsScalar()) continue;
                out.emplace(kv.first.Scalar(), kv.second.Scalar());
            }
            return out;
        }
        if (node->IsScalar()) {
            if (auto m = try_parse_dict_json(node->Scalar())) return *m;
        }
    }
    return default_value;
}

//------------------------------------------------------------------------------

std::vector<std::string> ConfigStore::get_list(const std::string& key, std::vector<std::string> default_value) const {
    std::string k{trim(key)};
    if (k.empty()) return default_value;
    std::string upper = to_upper(k);

    if (auto v = getenv_opt(k)) {
        if (auto l = try_parse_list_json(*v)) return *l;
    }
    if (upper != k) {
        if (auto v = getenv_opt(upper)) {
            if (auto l = try_parse_list_json(*v)) return *l;
        }
    }

    auto* node = lookup(upper);

    if (node) {
        if (node->IsSequence()) {
            std::vector<std::string> out;
            out.reserve(node->size());
            for (const auto& v : *node) {
                if (v.IsScalar()) out.push_back(v.Scalar());
            }
            return out;
        }
        if (node->IsScalar()) {
            if (auto l = try_parse_list_json(node->Scalar())) return *l;
        }
    }
    return default_value;
}

//------------------------------------------------------------------------------

void ConfigStore::print() const {
    std::vector<std::string> keys;
    keys.reserve(raw_.size());
    for (const auto& kv : raw_) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());

    std::size_t key_width = 0;
    for (const auto& k : keys) key_width = std::max(key_width, k.size());

    std::printf("\n");
    std::printf("Configuration (raw, %zu key%s)\n", raw_.size(), raw_.size() == 1 ? "" : "s");
    std::printf("------------------------------------------------------------\n");
    for (const auto& key : keys) {
        const auto& node = raw_.at(key);
        std::string value = format_node(node);
        if (node.IsScalar() && is_sensitive(key)) value = mask_value(value);
        std::printf("  %-*s : %s\n", static_cast<int>(key_width), key.c_str(), value.c_str());
    }
    std::printf("------------------------------------------------------------\n");
    std::fflush(stdout);
}

//------------------------------------------------------------------------------
// LocalYamlStore
//------------------------------------------------------------------------------

LocalYamlStore::LocalYamlStore(std::string path, std::shared_ptr<encrypt::Fernet> encrypter)
    : ConfigStore(std::move(encrypter)), path_(std::move(path)) {}

//------------------------------------------------------------------------------

bool LocalYamlStore::load() {
    if (path_.empty()) return true;
    std::ifstream f(path_);
    if (!f) {
        platform::log_warn("config: failed to open YAML file %s", path_.c_str());
        return false;
    }
    std::ostringstream buf;
    buf << f.rdbuf();
    return load_yaml_string(buf.str());
}

//------------------------------------------------------------------------------
// RemoteYamlStore
//------------------------------------------------------------------------------

RemoteYamlStore::RemoteYamlStore(std::string server, std::string token, std::string env,
                                 int timeout_ms, std::shared_ptr<encrypt::Fernet> encrypter)
    : ConfigStore(std::move(encrypter)),
      server_(std::move(server)),
      token_(std::move(token)),
      env_(std::move(env)),
      timeout_ms_(timeout_ms) {}

//------------------------------------------------------------------------------

bool RemoteYamlStore::load() {
    last_error_.clear();
    if (server_.empty()) { last_error_ = "empty 'server'"; return false; }
    if (token_.empty())  { last_error_ = "empty 'token'";  return false; }
    if (env_.empty())    { last_error_ = "empty 'env'";    return false; }

    std::string url;
    url.reserve(server_.size() + env_.size() + 64);
    url.append(server_);
    url.append("/api/v1/config/environments/");
    url.append(env_);
    url.append("/configs/resolved?is_yaml=true");

    std::vector<std::string> headers;
    headers.emplace_back(std::string{"X-Config-Token: "}.append(token_));

    client::HttpClient http;
    auto resp = http.get(url, timeout_ms_, headers);
    if (resp.status <= 0)                        { last_error_ = "http error: " + resp.body;       return false; }
    if (resp.status < 200 || resp.status >= 300) { last_error_ = std::to_string(resp.status) + ": " + resp.body; return false; }
    if (resp.body.empty())                       { last_error_ = "empty HTTP response body";       return false; }
    if (!load_yaml_string(resp.body))            { last_error_ = "failed to parse remote YAML body"; return false; }
    return true;
}

}
}
