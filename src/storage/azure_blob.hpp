#pragma once

// Azure Blob Storage backend, using Shared Key (HMAC-SHA256) authorization for
// the server's own requests and a service SAS for presigned read URLs. Azure is
// not S3-compatible -- different REST API, headers, and signing -- so unlike the
// many S3-compatible stores (R2, GCS XML API, MinIO, ...) that the AwsS3 backend
// covers via an endpoint override, it needs its own class. See
// src/storage/storage.hpp for the interface contract and AzureBlobConfig for how
// it is constructed from configuration.

#include "client/http_client.hpp"
#include "storage/storage.hpp"

#include <string>

namespace mirobody { namespace storage {

class AzureBlob : public Storage {
public:
    // Throws StorageError if the config is not fully populated (account / key /
    // container). Prefer AzureBlobConfig::open().
    explicit AzureBlob(AzureBlobConfig config);

    std::string bucket_name() const override { return config_.container; }
    std::string full_key(const std::string& key) const override;
    std::string put_object(const std::string& key,
                           const std::string& data,
                           const std::string& content_type) override;
    // Azure has Append Blobs, but a Block Blob written by put_object is not
    // appendable, so this keeps the base read-modify-write.
    std::string get_object(const std::string& key) override;
    void        delete_object(const std::string& key) override;
    std::vector<std::string> list_objects(const std::string& prefix,
                                          std::size_t max_keys) override;
    // A service SAS read URL (sv/sr/sp/se/spr/sig query string), valid for
    // expires_seconds. Computed locally -- no network call.
    std::string presigned_url(const std::string& key, int expires_seconds) const override;
    std::string public_url(const std::string& key) const override;

private:
    // "/<account>/<container>/<full_key>" plus any sorted query lines -- the
    // CanonicalizedResource the Shared Key signature covers. `query` is the
    // already-sorted "name:value" lines (each without the leading '\n').
    std::string canonical_resource(const std::string& full_key,
                                   const std::vector<std::string>& query_lines = {}) const;
    // The Authorization header value: "SharedKey <account>:<sig>".
    std::string authorization(const std::string& verb, const std::string& content_type,
                              std::size_t content_length,
                              const std::vector<std::string>& canonical_headers,
                              const std::string& canonical_resource) const;
    // "https://<account>.blob.<suffix>/<container>/<url-encoded full_key>".
    std::string object_url(const std::string& full_key) const;

    AzureBlobConfig    config_;
    std::string        host_;   // "<account>.blob.<endpoint_suffix>"
    client::HttpClient http_;
};

}}
