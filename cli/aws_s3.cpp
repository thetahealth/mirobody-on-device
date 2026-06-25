// Standalone CLI for the AWS S3 (and S3-compatible) storage backend.
//
// Exercises src/storage/aws_s3.* end-to-end — SigV4 signing, the libcurl
// transport, and presigned-URL generation — without the embedded server.
//
//   aws_s3 put charts/a.png --file a.png --content-type image/png
//   aws_s3 get charts/a.png --output a.png
//   aws_s3 presign charts/a.png --expires 900
//   aws_s3 url charts/a.png
//   aws_s3 delete charts/a.png
//   echo hello | aws_s3 put notes/hi.txt --content-type text/plain
//
// Configuration precedence (highest to lowest): --flag > env var > YAML
// (local --config / MIROBODY_CONFIG / ./config.yml, or remote CONFIG_*).
//
// YAML keys honored (see Config::s3 / the config.yml "Object Storage" block):
//   S3_KEY  S3_TOKEN  S3_REGION  S3_BUCKET  S3_PREFIX  S3_CDN  S3_ENDPOINT

#include "client/http_client.hpp"
#include "config/config.hpp"
#include "storage_cli.hpp"

#include <cstdio>
#include <memory>
#include <string>

int main(int argc, char** argv) {
    mirobody::client::HttpClient::global_init();

    auto make_store =
        [](const std::string& config_path, const std::string& /*name*/)
        -> std::unique_ptr<mirobody::storage::Storage> {
            mirobody::optional<std::string> path;
            if (!config_path.empty()) path = config_path;
            mirobody::Config cfg = mirobody::load_config(path);
            mirobody::storage::S3Config s3 = cfg.s3();
            if (!s3.configured()) {
                std::fprintf(stderr,
                    "aws_s3: not configured - set S3_KEY, S3_TOKEN, S3_BUCKET and "
                    "S3_REGION (via flags, env, or config.yml).\n");
                return nullptr;
            }
            return s3.open();
        };

    int rc = mirobody::tools::run_storage_cli("aws_s3", make_store, argc, argv);

    mirobody::client::HttpClient::global_cleanup();
    return rc;
}
