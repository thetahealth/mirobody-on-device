// Standalone CLI for the Alibaba Cloud OSS storage backend.
//
// Exercises src/storage/aliyun_oss.* end-to-end — the classic HMAC-SHA1
// signature, the libcurl transport, and presigned-URL generation — without the
// embedded server.
//
//   aliyun_oss put charts/a.png --file a.png --content-type image/png
//   aliyun_oss get charts/a.png --output a.png
//   aliyun_oss presign charts/a.png --expires 900
//   aliyun_oss url charts/a.png
//   aliyun_oss delete charts/a.png
//   aliyun_oss --name reports put metrics/q3.csv --file q3.csv   # ALI_OSS_*_REPORTS keys
//
// Configuration precedence (highest to lowest): --flag > env var > YAML
// (local --config / MIROBODY_CONFIG / ./config.yml, or remote CONFIG_*).
//
// YAML keys honored (see Config::oss / the config.yml "Object Storage" block);
// append an upper-cased "_<NAME>" suffix when --name is given:
//   ALI_OSS_ACCESS_KEY  ALI_OSS_SECRET_KEY  ALI_OSS_ENDPOINT
//   ALI_OSS_BUCKET_NAME  ALI_OSS_PREFIX  ALI_OSS_DOMAIN

#include "client/http_client.hpp"
#include "config/config.hpp"
#include "storage_cli.hpp"

#include <cstdio>
#include <memory>
#include <string>

int main(int argc, char** argv) {
    mirobody::client::HttpClient::global_init();

    auto make_store =
        [](const std::string& config_path, const std::string& name)
        -> std::unique_ptr<mirobody::storage::Storage> {
            mirobody::optional<std::string> path;
            if (!config_path.empty()) path = config_path;
            mirobody::Config cfg = mirobody::load_config(path);
            mirobody::storage::OssConfig oss = cfg.oss(name);
            if (!oss.configured()) {
                std::fprintf(stderr,
                    "aliyun_oss: not configured - set ALI_OSS_ACCESS_KEY, "
                    "ALI_OSS_SECRET_KEY, ALI_OSS_ENDPOINT and ALI_OSS_BUCKET_NAME%s "
                    "(via flags, env, or config.yml).\n",
                    name.empty() ? "" : " (with the --name suffix)");
                return nullptr;
            }
            return oss.open();
        };

    int rc = mirobody::tools::run_storage_cli("aliyun_oss", make_store, argc, argv);

    mirobody::client::HttpClient::global_cleanup();
    return rc;
}
