// fernet — encrypt / decrypt a string with a Fernet key (interactive key entry).
//
// Fernet is the symmetric authenticated-encryption scheme this server uses for
// secrets at rest (VENDOR_TOKEN_ENCRYPTION_KEY, FILE_ENCRYPTION_KEY; see
// src/config/fernet.hpp). Tokens are wire-compatible with Python's
// cryptography.fernet.Fernet. This tool encrypts/decrypts a single string with a
// key you supply, and can mint or derive a key.
//
// A Fernet key is 32 bytes as URL-safe base64 (the 44-char string genkey emits).
// If you'd rather use a memorable passphrase, pass --ascii: the key value is then
// taken as raw bytes and right-padded with 0x00 to 32 bytes (max 32; it won't
// truncate). `keyfrom` prints the resulting base64 key so you can store it.
//
// The key is read interactively from stdin (so it stays out of your shell
// history) unless you pass --key. Note stdin isn't echo-suppressed, so a typed
// key is visible on screen.
//
// Usage:
//   fernet genkey
//   fernet keyfrom [PASSPHRASE]                      # ASCII passphrase -> 44-char key
//   fernet encrypt [--key KEY] [--ascii] [TEXT]
//   fernet decrypt [--key KEY] [--ascii] [--ttl SECONDS] [TOKEN]
//
//   --key KEY       the key (44-char base64, or any string with --ascii); prompted
//                   on stdin if omitted
//   --ascii         treat the key as a raw ASCII passphrase, 0x00-padded to 32 bytes
//   --ttl SECONDS   (decrypt) reject tokens older than SECONDS
//   TEXT / TOKEN    the string to encrypt / token to decrypt; read from stdin
//                   if omitted
//
// Examples:
//   fernet genkey
//   fernet keyfrom "correct horse"                   # -> its 44-char Fernet key
//   fernet encrypt --ascii --key "correct horse" "my secret"
//   fernet encrypt "my secret"                        # prompts for a 44-char key
//   fernet decrypt --key "$KEY" "gAAAAAB..."

#include "config/fernet.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <string>

namespace {

void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage:\n"
        "  %s genkey\n"
        "  %s keyfrom [PASSPHRASE]\n"
        "  %s encrypt [--key KEY] [--ascii] [TEXT]\n"
        "  %s decrypt [--key KEY] [--ascii] [--ttl SECONDS] [TOKEN]\n"
        "\n"
        "  --key KEY      key (44-char base64, or any string with --ascii); prompted if omitted\n"
        "  --ascii        treat the key as a raw ASCII passphrase, 0x00-padded to 32 bytes\n"
        "  --ttl SECONDS  (decrypt) reject tokens older than SECONDS\n"
        "  TEXT / TOKEN   value to process; read from stdin if omitted\n",
        prog, prog, prog, prog);
}

// URL-safe base64 with '=' padding (the canonical Fernet-key form).
std::string b64url_encode(const unsigned char* p, std::size_t n) {
    static const char* A =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    std::size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        std::uint32_t v = (std::uint32_t(p[i]) << 16) |
                          (std::uint32_t(p[i + 1]) << 8) | p[i + 2];
        out += A[(v >> 18) & 63]; out += A[(v >> 12) & 63];
        out += A[(v >> 6) & 63];  out += A[v & 63];
    }
    const std::size_t rem = n - i;
    if (rem == 1) {
        std::uint32_t v = std::uint32_t(p[i]) << 16;
        out += A[(v >> 18) & 63]; out += A[(v >> 12) & 63]; out += "==";
    } else if (rem == 2) {
        std::uint32_t v = (std::uint32_t(p[i]) << 16) | (std::uint32_t(p[i + 1]) << 8);
        out += A[(v >> 18) & 63]; out += A[(v >> 12) & 63]; out += A[(v >> 6) & 63];
        out += "=";
    }
    return out;
}

// Derive a Fernet key from an ASCII passphrase: the bytes right-padded with 0x00
// to 32 bytes, base64url-encoded. Throws if the passphrase exceeds 32 bytes (it
// pads short ones but never truncates). Empty => an all-zero key.
std::string key_from_ascii(const std::string& ascii) {
    if (ascii.size() > 32) {
        throw std::runtime_error("passphrase is " + std::to_string(ascii.size()) +
                                 " bytes; max 32 (short ones are 0x00-padded, none truncated)");
    }
    std::string bytes = ascii;
    bytes.resize(32, '\0');   // right-pad with 0x00
    return b64url_encode(reinterpret_cast<const unsigned char*>(bytes.data()), 32);
}

// Strip trailing CR/LF so a value typed or piped with a newline works cleanly.
void rstrip_eol(std::string& s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) { s.pop_back(); }
}

// Read one line from stdin, prompting on stderr.
std::string read_line_stdin(const char* prompt) {
    std::fprintf(stderr, "%s", prompt);
    std::fflush(stderr);
    std::string line;
    std::getline(std::cin, line);
    rstrip_eol(line);
    return line;
}

// Read all remaining stdin (the text/token).
std::string read_rest_stdin() {
    return std::string((std::istreambuf_iterator<char>(std::cin)),
                       std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char** argv) {
    const char* prog = (argc > 0) ? argv[0] : "fernet";
    if (argc < 2) { print_usage(prog); return 2; }

    const std::string cmd = argv[1];
    if (cmd == "-h" || cmd == "--help") { print_usage(prog); return 0; }

    if (cmd == "genkey") {
        std::printf("%s\n", mirobody::encrypt::Fernet::generate_key().c_str());
        return 0;
    }

    // keyfrom: derive and print the 44-char Fernet key from an ASCII passphrase
    // (positional, else prompted). Handy for setting *_ENCRYPTION_KEY from a phrase.
    if (cmd == "keyfrom") {
        std::string phrase;
        bool have = false;
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "-h" || a == "--help") { print_usage(prog); return 0; }
            phrase = a; have = true;
        }
        if (!have) { phrase = read_line_stdin("Passphrase: "); }
        try {
            std::printf("%s\n", key_from_ascii(phrase).c_str());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s: %s\n", prog, e.what());
            return 1;
        }
        return 0;
    }

    if (cmd != "encrypt" && cmd != "decrypt") {
        std::fprintf(stderr, "%s: unknown command: %s\n", prog, cmd.c_str());
        print_usage(prog);
        return 2;
    }

    std::string  key;
    bool         have_key   = false;
    bool         ascii      = false;
    std::int64_t ttl        = 0;
    std::string  value;
    bool         have_value = false;

    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") { print_usage(prog); return 0; }
        else if (a == "--key" && i + 1 < argc) { key = argv[++i]; have_key = true; }
        else if (a == "--ascii") { ascii = true; }
        else if (a == "--ttl" && i + 1 < argc) { ttl = std::atoll(argv[++i]); }
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "%s: unknown argument: %s\n", prog, a.c_str());
            print_usage(prog);
            return 2;
        } else {
            value = a; have_value = true;
        }
    }

    // Key: --key wins; otherwise prompt + read a line from stdin.
    if (!have_key) {
        key = read_line_stdin(ascii ? "Passphrase: " : "Fernet key: ");
    }
    if (key.empty() && !ascii) {
        std::fprintf(stderr, "%s: no key provided\n", prog);
        return 2;
    }

    // Value: positional arg wins; otherwise the rest of stdin (trailing EOL trimmed
    // so a piped/typed newline doesn't become part of the plaintext/token).
    if (!have_value) { value = read_rest_stdin(); rstrip_eol(value); }

    try {
        const std::string fkey = ascii ? key_from_ascii(key) : key;
        mirobody::encrypt::Fernet f(fkey);
        const std::string out = (cmd == "encrypt") ? f.encrypt(value)
                                                    : f.decrypt(value, ttl);
        std::printf("%s\n", out.c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: %s\n", prog, e.what());
        return 1;
    }
    return 0;
}
