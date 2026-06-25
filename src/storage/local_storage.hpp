#pragma once

// Local filesystem object-storage backend. Implements the Storage interface
// over plain files under a root directory — no network, no credentials, no
// signing — for self-hosted and on-device deployments. See LocalConfig in
// src/storage/storage.hpp for how it is configured.

#include "storage/storage.hpp"

#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>

namespace mirobody { namespace storage {

class LocalStorage : public Storage {
public:
    // Creates `root` (recursively) if it does not exist. Throws StorageError if
    // `root` is empty or cannot be created, or if signing is configured
    // (url_prefix + secret both set) with a secret weaker than 16 non-whitespace
    // characters — a blank/trivial key would make signatures forgeable. Prefer
    // LocalConfig::open().
    explicit LocalStorage(LocalConfig config);

    // No bucket: the local filesystem keys per-user prefixes with "".
    std::string bucket_name() const override { return std::string(); }
    // Additionally throws StorageError if the key would escape `root`
    // (empty, "." / ".." segment, backslash, NUL, or absolute).
    std::string full_key(const std::string& key) const override;
    std::string put_object(const std::string& key,
                           const std::string& data,
                           const std::string& content_type) override;
    // Appends to the file in place -- no read-modify-write.
    std::string append_to_object(const std::string& key,
                                 const std::string& data,
                                 const std::string& content_type) override;
    std::string get_object(const std::string& key) override;
    void        delete_object(const std::string& key) override;
    std::vector<std::string> list_objects(const std::string& prefix,
                                          std::size_t max_keys) override;
    // A signed GET URL: object URL plus "?expires=<unix>&sig=<hmac>", where sig
    // is HMAC-SHA256(LOCAL_STORAGE_SECRET, "<key>\n<expires>"). The server's
    // storage mount recomputes the same HMAC to gate access for a bounded
    // window. Returns the plain (unsigned) URL — ignoring expires_seconds — when
    // signing is not enforced (no secret, or no url_prefix so this server
    // doesn't serve the object).
    std::string presigned_url(const std::string& key, int expires_seconds) const override;
    // The unsigned URL. THROWS StorageError when signing is enforced (a secret
    // and a url_prefix are both set): there is no resolvable unsigned URL then,
    // so callers must use presigned_url().
    std::string public_url(const std::string& key) const override;

private:
    // Absolute on-disk path for `full_key`.
    std::string disk_path(const std::string& full_key) const;
    // True when a GET requires a signature (secret + url_prefix both set).
    bool signing_enforced() const;
    // The unsigned object URL for a full key, regardless of signing. Shared by
    // public_url() (which gates on signing_enforced) and presigned_url().
    std::string object_url(const std::string& full_key) const;

    // A previously-minted signed URL and when to stop reusing it. presigned_url
    // hands back the cached URL until the clock passes refresh_after (half the
    // requested lifetime), so repeated calls return one stable, downstream-
    // cacheable URL rather than re-stamping a new expiry each time.
    struct SignedEntry {
        std::string url;
        std::time_t refresh_after;
    };

    LocalConfig config_;
    std::string abs_root_;   // resolved absolute path of config_.root

    // Signed-URL cache, keyed by "<full_key>|<expires_seconds>". Mutable and
    // mutex-guarded because presigned_url is const yet memoises its result.
    mutable std::mutex cache_mutex_;
    mutable std::unordered_map<std::string, SignedEntry> url_cache_;
};

}}
