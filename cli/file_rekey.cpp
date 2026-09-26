// file_rekey -- re-encrypt stored uploads under the newest FILE_ENCRYPTION_KEY.
//
// File encryption (FILE_ENCRYPTION_KEY) is a key LIST: the last key encrypts new
// writes, all keys decrypt (see src/storage/README.md). Rotating is zero
// downtime -- append a new key and old objects keep decrypting -- but to safely
// DROP an old key you must first move every object off it. This tool does that:
// it walks the object store, decrypts each upload / .meta / .trans with whatever
// configured key fits, and rewrites it encrypted under the newest key, IN PLACE
// (the object key is derived from FILE_KEY_SEED, which does not change on
// rotation, so paths and the `files` table are untouched). Once it reports no
// failures, the old key can be removed from FILE_ENCRYPTION_KEY.
//
// Usage:
//   file_rekey [--config PATH] [--user ID] [--max N] [--dry-run]
//
// Options:
//   --config PATH  YAML config to load (else MIROBODY_CONFIG / ./config.yml).
//   --user ID      re-encrypt only this user's objects (else every object).
//   --max N        cap the object scan (default 1000000); a warning prints if hit.
//   --dry-run      report what would be re-encrypted without writing.
//
// Configuration honored: LOCAL_STORAGE_* plus FILE_ENCRYPTION_KEY and
// FILE_KEY_SEED.

#include "client/http_client.hpp"
#include "config/config.hpp"
#include "storage/storage.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {

void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [--config PATH] [--user ID] [--max N] [--dry-run]\n"
        "  Re-encrypt stored uploads under the newest FILE_ENCRYPTION_KEY, in place.\n"
        "  --config PATH  YAML config (else MIROBODY_CONFIG / ./config.yml).\n"
        "  --user ID      only this user's objects (else all).\n"
        "  --max N        cap the object scan (default 1000000).\n"
        "  --dry-run      report without writing.\n",
        prog);
}

// Build the local-filesystem object store. Returns null after printing why when
// it is not configured.
std::unique_ptr<mirobody::storage::Storage> open_storage(const mirobody::Config& cfg) {
    if (cfg.local_storage().configured()) return cfg.local_storage().open();
    std::fprintf(stderr, "file_rekey: no object storage configured (set LOCAL_STORAGE_DIR)\n");
    return nullptr;
}

// Content-Type to (re)write an object as. The decrypting mount re-derives the
// served type from the key, so this only sets the stored metadata; keep it
// faithful anyway: text for a .trans transcript, NDJSON for a .meta sidecar,
// else the key's extension.
std::string content_type_for(const std::string& key) {
    if (mirobody::storage::is_trans_key(key)) return "text/plain; charset=utf-8";
    if (mirobody::storage::is_meta_key(key))  return "application/x-ndjson";
    return mirobody::storage::resolve_content_type(key, "");
}

}  // namespace

int main(int argc, char** argv) {
    const char* prog = (argc > 0) ? argv[0] : "file_rekey";

    std::string config_path;
    long long   user_id = 0;
    std::size_t max_keys = 1000000;
    bool        dry_run = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if      (a == "-h" || a == "--help")      { print_usage(prog); return 0; }
        else if (a == "--config" && i + 1 < argc) { config_path = argv[++i]; }
        else if (a == "--user"   && i + 1 < argc) { user_id = std::atoll(argv[++i]); }
        else if (a == "--max"    && i + 1 < argc) { max_keys = static_cast<std::size_t>(std::atoll(argv[++i])); }
        else if (a == "--dry-run")                { dry_run = true; }
        else { std::fprintf(stderr, "%s: unknown argument: %s\n", prog, a.c_str()); print_usage(prog); return 2; }
    }
    if (max_keys == 0) max_keys = 1000000;

    mirobody::client::HttpClient::global_init();
    int rc = 0;

    try {
        mirobody::optional<std::string> path;
        if (!config_path.empty()) path = config_path;
        mirobody::Config cfg = mirobody::load_config(path);

        std::unique_ptr<mirobody::storage::Storage> storage = open_storage(cfg);
        if (!storage) { mirobody::client::HttpClient::global_cleanup(); return 2; }

        if (cfg.file_encryption_keys.empty()) {
            std::fprintf(stderr, "file_rekey: FILE_ENCRYPTION_KEY is not set; nothing to re-encrypt\n");
            mirobody::client::HttpClient::global_cleanup();
            return 2;
        }

        // Configure the multi-key cipher + seed exactly as the server does (this
        // also runs the key/seed fingerprint guard). On a mismatch it refuses --
        // the same protection the server gives.
        const mirobody::storage::FileEncryptionResult fe =
            mirobody::storage::configure_file_encryption(
                *storage, cfg.file_encryption_keys, cfg.file_key_seed,
                cfg.file_url_prefix, cfg.file_url_base);
        if (!fe.ok) {
            std::fprintf(stderr, "file_rekey: %s\n", fe.error.c_str());
            mirobody::client::HttpClient::global_cleanup();
            return 1;
        }

        std::fprintf(stderr, "file_rekey: %zu key%s configured; rewriting under the newest%s\n",
                     cfg.file_encryption_keys.size(), cfg.file_encryption_keys.size() == 1 ? "" : "s",
                     dry_run ? " (dry run)" : "");

        // Enumerate: one user's prefix, or every object under the store prefix.
        const std::vector<std::string> keys = (user_id > 0)
            ? storage->list_user_objects(static_cast<std::int64_t>(user_id), max_keys)
            : storage->list_objects("", max_keys);
        if (keys.size() >= max_keys) {
            std::fprintf(stderr, "file_rekey: WARNING scan hit the --max cap (%zu); some objects may "
                                 "be unprocessed -- raise --max and re-run\n", max_keys);
        }

        std::size_t reencrypted = 0, skipped = 0, failed = 0;
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const std::string& key = keys[i];

            // Decrypt with whatever configured key fits. A plaintext object (a
            // chart, the .file-encryption-check marker) -- or one encrypted under
            // a key no longer configured -- fails here and is skipped.
            std::string plain;
            try {
                plain = storage->get_object_decrypted(key);
            } catch (const mirobody::storage::StorageError&) {
                ++skipped;
                continue;
            }

            if (dry_run) { ++reencrypted; continue; }

            // Rewrite in place, encrypted under the newest key.
            try {
                storage->put_object_encrypted(key, plain, content_type_for(key));
                ++reencrypted;
            } catch (const mirobody::storage::StorageError& e) {
                std::fprintf(stderr, "file_rekey: re-encrypt failed for '%s': %s\n", key.c_str(), e.what());
                ++failed;
            }
        }

        std::fprintf(stderr,
                     "file_rekey: %s %zu object%s, skipped %zu (plaintext or under a removed key), %zu failed\n",
                     dry_run ? "would re-encrypt" : "re-encrypted",
                     reencrypted, reencrypted == 1 ? "" : "s", skipped, failed);
        if (failed > 0) rc = 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "file_rekey: %s\n", e.what());
        rc = 1;
    }

    mirobody::client::HttpClient::global_cleanup();
    return rc;
}
