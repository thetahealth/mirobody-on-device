// jwt_keygen — generate an RSA keypair for RS256 JWT signing.
//
// The server signs OAuth/MCP access tokens with RS256 when JWT_PRIVATE_KEY is
// set, and publishes the matching public key as a JWKS at /.well-known/jwks.json
// so third-party resource servers can verify them (see src/oauth/README.md).
// This tool mints a fresh keypair and, by default, prints a ready-to-paste
// config.yml snippet. It also reports the key id (RFC 7638 JWK thumbprint) the
// server will advertise.
//
// Usage:
//   jwt_keygen [--bits N] [--out PREFIX] [--pem] [--jwks]
//
// Options:
//   --bits N      RSA modulus size in bits (default 2048; e.g. 3072, 4096)
//   --out PREFIX  write PREFIX.key (private PEM) and PREFIX.pub (public PEM)
//                 instead of printing; the private file is created 0600 on POSIX
//   --pem         print raw private then public PEM to stdout (not the snippet)
//   --jwks        also print the JWKS document (the /.well-known/jwks.json body)
//
// Examples:
//   jwt_keygen                         # -> config.yml snippet on stdout, kid on stderr
//   jwt_keygen --bits 3072 --jwks      # snippet + the JWKS body
//   jwt_keygen --out jwt_signing       # writes jwt_signing.key + jwt_signing.pub
//   eval "$(jwt_keygen --pem)"         # (or redirect the PEMs to files yourself)

#include "jwt/jwt.hpp"

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {

void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [--bits N] [--out PREFIX] [--pem] [--jwks]\n"
        "  --bits N      RSA key size in bits (default 2048)\n"
        "  --out PREFIX  write PREFIX.key (private) + PREFIX.pub (public) instead of printing\n"
        "  --pem         print raw private then public PEM to stdout\n"
        "  --jwks        also print the JWKS document (.well-known/jwks.json body)\n",
        prog);
}

// Export an EVP_PKEY half to a PEM string via a memory BIO. `writer` is the
// matching PEM_write_bio_* call. Returns "" on failure.
std::string pem_export(EVP_PKEY* key, int (*writer)(BIO*, EVP_PKEY*)) {
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio) return std::string();
    std::string out;
    if (writer(bio, key) == 1) {
        BUF_MEM* mem = nullptr;
        BIO_get_mem_ptr(bio, &mem);
        if (mem && mem->data) out.assign(mem->data, mem->length);
    }
    BIO_free(bio);
    return out;
}

int write_priv(BIO* b, EVP_PKEY* k) {
    // Unencrypted PKCS#8 (-----BEGIN PRIVATE KEY-----), which the server's
    // parse_private_pem reads.
    return PEM_write_bio_PrivateKey(b, k, nullptr, nullptr, 0, nullptr, nullptr);
}
int write_pub(BIO* b, EVP_PKEY* k) {
    // SubjectPublicKeyInfo (-----BEGIN PUBLIC KEY-----).
    return PEM_write_bio_PUBKEY(b, k);
}

// Indent every line of `pem` by two spaces, for a YAML block scalar.
std::string yaml_indent(const std::string& pem) {
    std::string out;
    out.reserve(pem.size() + pem.size() / 32);
    out += "  ";
    for (std::size_t i = 0; i < pem.size(); ++i) {
        out += pem[i];
        if (pem[i] == '\n' && i + 1 < pem.size()) out += "  ";
    }
    if (!out.empty() && out.back() != '\n') out += '\n';
    return out;
}

bool write_file(const std::string& path, const std::string& data, bool is_private) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
#ifndef _WIN32
    if (is_private) chmod(path.c_str(), S_IRUSR | S_IWUSR);   // 0600
#else
    (void)is_private;
#endif
    return ok;
}

EVP_PKEY* generate_rsa(int bits) {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!ctx) return nullptr;
    EVP_PKEY* key = nullptr;
    if (EVP_PKEY_keygen_init(ctx) == 1 &&
        EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, bits) > 0) {
        EVP_PKEY_keygen(ctx, &key);
    }
    EVP_PKEY_CTX_free(ctx);
    return key;
}

}  // namespace

int main(int argc, char** argv) {
    const char* prog = (argc > 0) ? argv[0] : "jwt_keygen";

    int bits = 2048;
    std::string out_prefix;
    bool raw_pem = false;
    bool show_jwks = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") { print_usage(prog); return 0; }
        else if (a == "--bits" && i + 1 < argc) { bits = std::atoi(argv[++i]); }
        else if (a == "--out"  && i + 1 < argc) { out_prefix = argv[++i]; }
        else if (a == "--pem")  { raw_pem = true; }
        else if (a == "--jwks") { show_jwks = true; }
        else { std::fprintf(stderr, "%s: unknown argument: %s\n", prog, a.c_str()); print_usage(prog); return 2; }
    }

    if (bits < 2048) {
        std::fprintf(stderr, "%s: --bits must be >= 2048 (got %d)\n", prog, bits);
        return 2;
    }

    EVP_PKEY* key = generate_rsa(bits);
    if (!key) { std::fprintf(stderr, "%s: RSA key generation failed\n", prog); return 1; }

    const std::string priv_pem = pem_export(key, write_priv);
    const std::string pub_pem  = pem_export(key, write_pub);
    EVP_PKEY_free(key);
    if (priv_pem.empty() || pub_pem.empty()) {
        std::fprintf(stderr, "%s: failed to export PEM\n", prog);
        return 1;
    }

    // Derive the kid + JWKS the server would publish for this key, so the
    // operator can see them up front.
    std::string kid, jwks;
    try {
        mirobody::jwt::JwtRs256::Options o;
        o.private_key_pem = priv_pem;
        o.public_key_pem  = pub_pem;
        mirobody::jwt::JwtRs256 j(std::move(o));
        kid  = j.kid();
        jwks = j.jwks_json();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: warning: could not derive kid/JWKS: %s\n", prog, e.what());
    }

    if (!out_prefix.empty()) {
        const std::string priv_path = out_prefix + ".key";
        const std::string pub_path  = out_prefix + ".pub";
        if (!write_file(priv_path, priv_pem, /*is_private=*/true) ||
            !write_file(pub_path, pub_pem, /*is_private=*/false)) {
            std::fprintf(stderr, "%s: failed to write key files\n", prog);
            return 1;
        }
        std::fprintf(stderr, "wrote %s (private, keep secret) and %s (public)\n",
                     priv_path.c_str(), pub_path.c_str());
    } else if (raw_pem) {
        std::fputs(priv_pem.c_str(), stdout);
        std::fputs(pub_pem.c_str(), stdout);
    } else {
        // Ready-to-paste config.yml block. JWT_PUBLIC_KEY is optional (the
        // server derives it from the private key) but emitted for clarity.
        std::printf("JWT_PRIVATE_KEY: |\n%s", yaml_indent(priv_pem).c_str());
        std::printf("JWT_PUBLIC_KEY: |\n%s",  yaml_indent(pub_pem).c_str());
    }

    if (!kid.empty()) std::fprintf(stderr, "kid (RFC 7638 thumbprint): %s\n", kid.c_str());
    if (show_jwks && !jwks.empty()) std::printf("%s\n", jwks.c_str());

    return 0;
}
