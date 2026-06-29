#pragma once

// Alibaba Cloud OSS object-storage backend, using the classic (HMAC-SHA1)
// signature. See src/storage/storage.hpp for the interface contract and
// OssConfig for how it is constructed from configuration.

#include "client/http_client.hpp"
#include "storage/storage.hpp"

#include <string>

namespace mirobody { namespace storage {

class AliyunOss : public Storage {
public:
    // Throws StorageError if the config is not fully populated (access_key_id /
    // secret_access_key / endpoint / bucket). Prefer OssConfig::open().
    explicit AliyunOss(OssConfig config);

    std::string bucket_name() const override { return config_.bucket; }
    std::string full_key(const std::string& key) const override;
    std::string put_object(const std::string& key,
                           const std::string& data,
                           const std::string& content_type) override;
    // Native AppendObject (POST ?append&position=N). Appends in place --
    // no read-modify-write -- for objects created by append; an object
    // created by put_object (type Normal) is not appendable, so that case
    // falls back to the base implementation.
    std::string append_to_object(const std::string& key,
                                 const std::string& data,
                                 const std::string& content_type) override;
    std::string get_object(const std::string& key) override;
    void        delete_object(const std::string& key) override;
    std::vector<std::string> list_objects(const std::string& prefix,
                                          std::size_t max_keys) override;
    std::string presigned_url(const std::string& key, int expires_seconds) const override;
    std::string public_url(const std::string& key) const override;

private:
    // "/<bucket>/<full_key>" — the CanonicalizedResource the signature covers.
    std::string canonical_resource(const std::string& full_key) const;
    // "https://<bucket>.<endpoint>/<url-encoded full_key>".
    std::string object_url(const std::string& full_key) const;

    OssConfig           config_;
    std::string         host_;          // "<bucket>.<endpoint>"
    client::HttpClient  http_;
};

}}
