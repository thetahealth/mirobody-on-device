// Shared command runner for the object-storage debug CLIs under cli/
// (aws_s3, aliyun_oss). Each CLI builds its backend-specific Storage from
// config, then hands argv to run_storage_cli() here, which parses one
// sub-command and executes it. Header-only, matching event_printer.hpp.
//
// Sub-commands (key is passed WITHOUT the configured prefix; the backend
// prepends it):
//   put <key>      upload; body from --data, --file <path>, or stdin
//   get <key>      download; bytes to stdout, or --output <path>
//   delete <key>   remove (idempotent)
//   url <key>      print the public URL
//   presign <key>  print a presigned GET URL (--expires <seconds>, default 3600)

#pragma once

#include "config/config.hpp"
#include "storage/storage.hpp"
#include "event_printer.hpp"   // prepare_windows_console, stdin_is_tty

#include "compat/cxx11.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <string>

#if defined(_WIN32)
#include <fcntl.h>   // _O_BINARY
#include <io.h>      // _setmode, _fileno
#endif

namespace mirobody { namespace tools {

// Build a Storage from the loaded config. `config_path` is the resolved
// --config value (empty => default discovery); `name` is the optional instance
// name (used by the OSS backend's suffix lookup, ignored by S3). Returns null
// after printing its own error when the backend is not configured.
using StorageFactory =
    std::function<std::unique_ptr<storage::Storage>(const std::string& config_path,
                                                    const std::string& name)>;

namespace detail {

inline void storage_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [options] <command> <key>\n"
        "\n"
        "Commands:\n"
        "  put <key>       Upload an object. Body from --data, --file, or stdin.\n"
        "  get <key>       Download an object to stdout (or --output <path>).\n"
        "  delete <key>    Delete an object (idempotent).\n"
        "  url <key>       Print the object's public URL.\n"
        "  presign <key>   Print a presigned GET URL.\n"
        "\n"
        "Options:\n"
        "  --config <path>       Local YAML to load (overrides MIROBODY_CONFIG / ./config.yml).\n"
        "  --name <name>         Instance name for suffixed config keys (OSS only).\n"
        "  --data <string>       Inline body for `put` (alternative to --file / stdin).\n"
        "  --file <path>         Read the `put` body from a file.\n"
        "  --output <path>       Write the `get` body to a file instead of stdout.\n"
        "  --content-type <ct>   Content-Type for `put` (default application/octet-stream).\n"
        "  --expires <seconds>   Lifetime for `presign` (default 3600).\n"
        "  -h, --help            Show this help.\n",
        prog);
}

// Read a whole file into a string (binary). Returns false on open failure.
inline bool read_file(const std::string& path, std::string& out) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) return false;
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return !in.bad();
}

inline std::string read_stdin_binary() {
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    std::string out;
    char buf[65536];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), stdin)) out.append(buf, n);
    return out;
}

inline void write_stdout_binary(const std::string& body) {
#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::fwrite(body.data(), 1, body.size(), stdout);
}

}

//------------------------------------------------------------------------------

inline int run_storage_cli(const char* cli_name, const StorageFactory& make_store,
                           int argc, char** argv) {
    std::string config_path, name;
    std::string command, key;
    std::string data, file, output, content_type = "application/octet-stream";
    bool have_data = false;
    int expires = 3600;

    auto need = [&](int& i, const char* what) -> std::string {
        if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", what); std::exit(2); }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "-h" || a == "--help")  { detail::storage_usage(argv[0]); return 0; }
        else if (a == "--config")             { config_path = need(i, "--config"); }
        else if (a == "--name")               { name = need(i, "--name"); }
        else if (a == "--data")               { data = need(i, "--data"); have_data = true; }
        else if (a == "--file")               { file = need(i, "--file"); }
        else if (a == "--output")             { output = need(i, "--output"); }
        else if (a == "--content-type")       { content_type = need(i, "--content-type"); }
        else if (a == "--expires")            { expires = std::atoi(need(i, "--expires").c_str()); }
        else if (!a.empty() && a.front() == '-') {
            std::fprintf(stderr, "unknown option: %.*s\n", static_cast<int>(a.size()), a.data());
            detail::storage_usage(argv[0]); return 2;
        }
        else if (command.empty())             { command = std::string{a}; }
        else if (key.empty())                 { key = std::string{a}; }
        else {
            std::fprintf(stderr, "unexpected argument: %.*s\n", static_cast<int>(a.size()), a.data());
            return 2;
        }
    }

    if (command.empty() || key.empty()) {
        std::fprintf(stderr, "expected a command and a key\n\n");
        detail::storage_usage(argv[0]);
        return 2;
    }

    mirobody::tools::prepare_windows_console();

    std::unique_ptr<storage::Storage> store = make_store(config_path, name);
    if (!store) return 2;   // factory already printed why

    try {
        if (command == "put") {
            std::string body;
            if (have_data) {
                body = data;
            } else if (!file.empty()) {
                if (!detail::read_file(file, body)) {
                    std::fprintf(stderr, "%s: cannot read --file '%s'\n", cli_name, file.c_str());
                    return 1;
                }
            } else if (!mirobody::tools::stdin_is_tty()) {
                body = detail::read_stdin_binary();
            } else {
                std::fprintf(stderr, "%s put: no body - pass --data, --file, or pipe via stdin\n", cli_name);
                return 2;
            }
            std::string stored = store->put_object(key, body, content_type);
            std::fprintf(stdout, "%s\n", stored.c_str());
        } else if (command == "get") {
            std::string body = store->get_object(key);
            if (!output.empty()) {
                std::ofstream out(output.c_str(), std::ios::binary | std::ios::trunc);
                if (!out) { std::fprintf(stderr, "%s: cannot open --output '%s'\n", cli_name, output.c_str()); return 1; }
                out.write(body.data(), static_cast<std::streamsize>(body.size()));
            } else {
                detail::write_stdout_binary(body);
            }
        } else if (command == "delete") {
            store->delete_object(key);
            std::fprintf(stderr, "deleted %s\n", key.c_str());
        } else if (command == "url") {
            std::fprintf(stdout, "%s\n", store->public_url(key).c_str());
        } else if (command == "presign") {
            std::fprintf(stdout, "%s\n", store->presigned_url(key, expires).c_str());
        } else {
            std::fprintf(stderr, "unknown command: %s\n\n", command.c_str());
            detail::storage_usage(argv[0]);
            return 2;
        }
    } catch (const storage::StorageError& e) {
        std::fprintf(stderr, "%s: %s\n", cli_name, e.what());
        return 1;
    }
    return 0;
}

}}
