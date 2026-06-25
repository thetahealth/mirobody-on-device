// Standalone CLI for the Azure Blob Storage backend.
//
// Exercises src/storage/azure_blob.* end-to-end -- Shared Key signing, the
// libcurl transport, and service-SAS URL generation -- without the embedded
// server.
//
//   azure_blob put charts/a.png --file a.png --content-type image/png
//   azure_blob get charts/a.png --output a.png
//   azure_blob presign charts/a.png --expires 900
//   azure_blob url charts/a.png
//   azure_blob delete charts/a.png
//   echo hello | azure_blob put notes/hi.txt --content-type text/plain
//
// Configuration precedence (highest to lowest): --flag > env var > YAML
// (local --config / MIROBODY_CONFIG / ./config.yml, or remote CONFIG_*).
//
// YAML keys honored (see Config::azure_blob / the config.yml "Object Storage"
// block): AZURE_BLOB_ACCOUNT  AZURE_BLOB_KEY  AZURE_BLOB_CONTAINER
//         AZURE_BLOB_PREFIX  AZURE_BLOB_ENDPOINT_SUFFIX  AZURE_BLOB_CDN

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
            mirobody::storage::AzureBlobConfig az = cfg.azure_blob();
            if (!az.configured()) {
                std::fprintf(stderr,
                    "azure_blob: not configured - set AZURE_BLOB_ACCOUNT, AZURE_BLOB_KEY and "
                    "AZURE_BLOB_CONTAINER (via flags, env, or config.yml).\n");
                return nullptr;
            }
            return az.open();
        };

    int rc = mirobody::tools::run_storage_cli("azure_blob", make_store, argc, argv);

    mirobody::client::HttpClient::global_cleanup();
    return rc;
}
