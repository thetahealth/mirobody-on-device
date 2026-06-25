#pragma once

// AWS S3 (and S3-compatible) object-storage backend, signing requests with AWS
// Signature Version 4. See src/storage/storage.hpp for the interface contract
// and S3Config for how it is constructed from configuration.

#include "client/http_client.hpp"
#include "storage/storage.hpp"

#include <string>

namespace mirobody { namespace storage {

class AwsS3 : public Storage {
public:
    // Throws StorageError if the config is not fully populated
    // (access_key / secret_key / bucket / region). Prefer S3Config::open().
    explicit AwsS3(S3Config config);

    std::string bucket_name() const override { return config_.bucket; }
    std::string full_key(const std::string& key) const override;
    std::string put_object(const std::string& key,
                           const std::string& data,
                           const std::string& content_type) override;
    // append_to_object: inherits the base read-modify-write. S3 has a native
    // append (PutObject + x-amz-write-offset-bytes) only on directory buckets
    // (S3 Express One Zone), which this general-purpose client doesn't target.
    std::string get_object(const std::string& key) override;
    void        delete_object(const std::string& key) override;
    std::vector<std::string> list_objects(const std::string& prefix,
                                          std::size_t max_keys) override;
    std::string presigned_url(const std::string& key, int expires_seconds) const override;
    std::string public_url(const std::string& key) const override;

private:
    // Canonical (percent-encoded, leading-slash) request path for `full_key`.
    std::string canonical_uri(const std::string& full_key) const;

    S3Config            config_;
    std::string         host_;          // Host header / URL authority
    bool                path_style_ = false;
    client::HttpClient  http_;
};

}}
