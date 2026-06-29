#pragma once

#include "config/fernet.hpp"

#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <memory>
#include "compat/cxx11.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace mirobody { namespace utils {

// Read-only configuration store. Holds a key/value bag loaded from YAML and
// decrypts any string values that look like Fernet tokens. Concrete sources
// derive from this base and implement load(): see LocalYamlStore (file on
// disk) and RemoteYamlStore (HTTP endpoint).
//
// Data model and getter semantics mirror the Python utility in
// a007-opensource/mirobody/utils/config/config.py so the same YAML files
// and the same CONFIG_ENCRYPTION_KEY work on both sides. Unlike the Python
// version, this implementation never auto-encrypts and never writes back to
// the source: it only decrypts on load.
//
// Not thread-safe: callers should finish all load() / load_yaml_string /
// absorb calls before sharing the instance across threads.
class ConfigStore {
public:
    virtual ~ConfigStore() = default;

    // Sets or replaces the encrypter. Only affects subsequent ingestion;
    // values already in the store keep whatever state they had.
    void set_encrypter(std::shared_ptr<encrypt::Fernet> encrypter);

    // Mirror of Python's Config.get_fernet_key. Trims whitespace, truncates
    // to the first 32 bytes, right-pads with ASCII '0' to exactly 32 bytes,
    // then URL-safe base64 encodes. The result is a 44-char string suitable
    // for constructing a Fernet instance.
    static std::string derive_fernet_key(const std::string& secret);

    // -------------------- loading --------------------

    // Source-specific population step. Returns true on success; on failure
    // implementations log and leave the store unchanged.
    virtual bool load() = 0;

    // Ingest a YAML body already in memory. Called by derived load()
    // implementations after they have fetched the body; also useful for
    // tests and ad-hoc composition. Repeat calls merge with last-wins
    // semantics.
    bool load_yaml_string(const std::string& content);

    // Copy every entry from `other` into this store, overwriting any
    // existing keys. Used to layer a lower-priority source (e.g. remote)
    // under a higher-priority one: absorb the lower first, then load the
    // higher, so the higher overwrites and wins.
    void absorb(const ConfigStore& other);

    // -------------------- getters --------------------

    // True if either the environment (raw or upper-cased key) or the loaded
    // configuration supplies the key.
    bool has(const std::string& key) const;

    // Scalar getters. Lookup order: getenv(key) > getenv(upper(key)) >
    // loaded config (upper(key)) > default. Empty values count as unset at
    // every tier, so an empty env var / YAML scalar yields the default, not "".
    std::string  get_str (const std::string& key, const std::string& default_value = {}) const;
    std::int64_t get_int (const std::string& key, std::int64_t default_value = 0) const;
    bool         get_bool(const std::string& key, bool default_value = false) const;

    // Map and list getters. Accept either a YAML map/sequence node or a
    // string scalar that parses as JSON. Only string-valued entries are
    // returned in the map/list; numeric and boolean leaves are stringified.
    std::unordered_map<std::string, std::string> get_dict(
        const std::string& key,
        std::unordered_map<std::string, std::string> default_value = {}) const;

    std::vector<std::string> get_list(
        const std::string& key,
        std::vector<std::string> default_value = {}) const;

    // Dumps every loaded top-level key to stdout in alphabetical order, with
    // values that look like secrets masked (keys matching _KEY / _PASSWORD /
    // _PASS / _PWD / _SECRET / _SK / _TOKEN and not ending in _URL: same
    // pattern Python's loader uses to flag sensitive values). Non-scalar
    // nodes are shown as `<list of N items>` / `<map of N entries>`.
    // Environment-variable overrides are not reflected: only what was
    // actually ingested from YAML/remote sources is printed.
    void print() const;

protected:
    explicit ConfigStore(std::shared_ptr<encrypt::Fernet> encrypter = {});

    void ingest_yaml_root(const YAML::Node& root);
    const YAML::Node* lookup(const std::string& upper_key) const;

    std::shared_ptr<encrypt::Fernet> encrypter_;
    std::unordered_map<std::string, YAML::Node> raw_;
};

//------------------------------------------------------------------------------

// YAML loaded from a file on disk. A default-constructed instance (or one
// built with an empty path) is an empty store whose load() is a no-op;
// callers can still feed it via load_yaml_string or absorb.
class LocalYamlStore : public ConfigStore {
public:
    LocalYamlStore() = default;
    explicit LocalYamlStore(std::string path,
                            std::shared_ptr<encrypt::Fernet> encrypter = {});

    const std::string& path() const { return path_; }

    // Reads the file at `path()` and ingests it. Returns false (and logs a
    // warning) on I/O or parse failure. Returns true with no effect when
    // the path is empty.
    bool load() override;

private:
    std::string path_;
};

//------------------------------------------------------------------------------

// YAML pulled from a config server. load() issues
//   GET {server}/api/v1/config/environments/{env}/configs/resolved?is_yaml=true
// with header `X-Config-Token: {token}` and feeds the response body into
// load_yaml_string. Inspect last_error() for the failure reason when
// load() returns false.
class RemoteYamlStore : public ConfigStore {
public:
    RemoteYamlStore(std::string server,
                    std::string token,
                    std::string env,
                    int timeout_ms = 10000,
                    std::shared_ptr<encrypt::Fernet> encrypter = {});

    bool load() override;

    // Failure reason from the last load() invocation. Empty if the last
    // call succeeded or load() has not been called.
    const std::string& last_error() const { return last_error_; }

private:
    std::string server_;
    std::string token_;
    std::string env_;
    int         timeout_ms_;
    std::string last_error_;
};

}
}
