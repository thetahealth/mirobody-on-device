#include "storage/local_storage.hpp"
#include "storage/sign.hpp"   // uri_encode, hmac_sha256, hex_encode

#include "platform/log.hpp"

#include <array>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
#include <direct.h>
#include <stdlib.h>   // _fullpath, _MAX_PATH
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // FindFirstFileA (directory walk for list_objects)
#else
#include <dirent.h>   // opendir/readdir (directory walk for list_objects)
#include <limits.h>   // PATH_MAX
#include <stdlib.h>   // realpath
#include <unistd.h>
#endif

#include <algorithm>

namespace mirobody { namespace storage {

namespace {

//------------------------------------------------------------------------------

bool is_dir(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    return (st.st_mode & S_IFDIR) != 0;
}

bool is_regular_file(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    return (st.st_mode & S_IFREG) != 0;
}

//------------------------------------------------------------------------------

int make_dir(const std::string& path) {
#if defined(_WIN32)
    return _mkdir(path.c_str());
#else
    return ::mkdir(path.c_str(), 0755);
#endif
}

// Create `path` and any missing parents (the `mkdir -p` behaviour the C++11
// standard library lacks without <filesystem>). Accepts '/'-separated paths;
// returns true if the directory exists afterwards.
bool make_dirs(const std::string& path) {
    if (path.empty() || is_dir(path)) return is_dir(path);
    std::string acc;
    acc.reserve(path.size());
    for (std::size_t i = 0; i < path.size(); ++i) {
        const char c = path[i];
        if (c == '/' || c == '\\') {
            // Don't try to mkdir a drive root ("C:") or an empty leading token.
            if (!acc.empty() && acc.back() != ':' && !is_dir(acc)) {
                if (make_dir(acc) != 0 && !is_dir(acc)) return false;
            }
            acc.push_back('/');
        } else {
            acc.push_back(c);
        }
    }
    if (!is_dir(acc)) {
        if (make_dir(acc) != 0 && !is_dir(acc)) return false;
    }
    return is_dir(acc);
}

//------------------------------------------------------------------------------

// Absolute path of `path`, with separators normalised to '/'. Falls back to the
// input unchanged if the platform call fails.
std::string absolute_path(const std::string& path) {
    std::string abs = path;
#if defined(_WIN32)
    char buf[_MAX_PATH];
    if (_fullpath(buf, path.c_str(), sizeof(buf)) != nullptr) abs = buf;
#else
    char buf[PATH_MAX];
    if (::realpath(path.c_str(), buf) != nullptr) abs = buf;
#endif
    for (std::size_t i = 0; i < abs.size(); ++i) {
        if (abs[i] == '\\') abs[i] = '/';
    }
    while (abs.size() > 1 && abs.back() == '/') abs.pop_back();
    return abs;
}

//------------------------------------------------------------------------------

std::string trim_slashes(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == '/' || s[b] == '\\')) ++b;
    while (e > b && (s[e - 1] == '/' || s[e - 1] == '\\')) --e;
    return std::string(s.substr(b, e - b));
}

std::string trim_trailing_slash(const std::string& s) {
    std::string out = s;
    while (!out.empty() && out.back() == '/') out.pop_back();
    return out;
}

// Reject anything that could escape the root: empty, NUL, or a "." / ".."
// path segment. Backslashes at the ends are folded by trim_slashes; guard
// interior ones too.
void validate_key_segments(const std::string& fk) {
    if (fk.empty() || fk.find('\0') != std::string::npos) {
        throw StorageError("LocalStorage: invalid object key");
    }
    std::size_t i = 0;
    while (i <= fk.size()) {
        std::size_t slash = fk.find('/', i);
        std::string seg = fk.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
        if (seg == "." || seg == ".." || seg.find('\\') != std::string::npos) {
            throw StorageError("LocalStorage: invalid object key '" + fk + "'");
        }
        if (slash == std::string::npos) break;
        i = slash + 1;
    }
}

// Minimum number of non-whitespace characters required of LOCAL_STORAGE_SECRET
// when signing is in play. Long enough to rule out blank / typo / one-char keys
// that would make HMAC signatures forgeable; the placeholder shipped in
// config.yml clears it, so the default config still starts.
const std::size_t kMinSecretChars = 16;

// Count of non-whitespace characters in `s` (so a whitespace-only secret counts
// as 0 and a padded one is judged on its real content).
std::size_t nonblank_len(const std::string& s) {
    std::size_t n = 0;
    for (char c : s) {
        if (!std::isspace(static_cast<unsigned char>(c))) ++n;
    }
    return n;
}

//------------------------------------------------------------------------------

// Append every regular file under `dir` (recursively) to `out` as a
// '/'-separated key relative to the walk's start; `rel` is the relative path
// of `dir` itself ("" at the start). C++11 has no <filesystem>, so this walks
// with the platform directory API.
void walk_files(const std::string& dir, const std::string& rel, std::vector<std::string>& out) {
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "/*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        const std::string child_rel = rel.empty() ? name : rel + "/" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            walk_files(dir + "/" + name, child_rel, out);
        } else {
            out.push_back(child_rel);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return;
    while (struct dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        const std::string child     = dir + "/" + name;
        const std::string child_rel = rel.empty() ? name : rel + "/" + name;
        if (is_dir(child)) {
            walk_files(child, child_rel, out);
        } else if (is_regular_file(child)) {
            out.push_back(child_rel);
        }
    }
    ::closedir(d);
#endif
}

}

//------------------------------------------------------------------------------

LocalStorage::LocalStorage(LocalConfig config) : config_(std::move(config)) {
    if (config_.root.empty()) {
        throw StorageError("LocalStorage: root directory (LOCAL_STORAGE_DIR) is required");
    }
    if (!make_dirs(config_.root)) {
        throw StorageError("LocalStorage: cannot create root directory '" + config_.root + "'");
    }
    // Reject a blank / too-short signing secret up front (see
    // LocalConfig::validate_signing_secret); it would otherwise sign with a
    // worthless, forgeable key while reporting access as "secured".
    config_.validate_signing_secret();
    abs_root_ = absolute_path(config_.root);
}

//------------------------------------------------------------------------------

std::string LocalStorage::full_key(const std::string& key) const {
    const std::string k = trim_slashes(key);
    if (k.empty()) throw StorageError("LocalStorage: empty object key");
    // The configured key prefix (LOCAL_STORAGE_PREFIX) is prepended like the
    // S3/OSS prefixes -- idempotently, so a key already carrying it (any
    // API-returned key) passes through unchanged. On disk the object lives
    // under `root`/<prefix>/. url_prefix remains a URL concern only (see
    // public_url), never part of the stored path.
    const std::string cfg = trim_slashes(config_.prefix);
    const std::string cfgp = cfg.empty() ? std::string() : cfg + "/";
    const std::string fk =
        (!cfgp.empty() && k.compare(0, cfgp.size(), cfgp) == 0) ? k : cfgp + k;

    validate_key_segments(fk);
    return fk;
}

//------------------------------------------------------------------------------

std::string LocalStorage::disk_path(const std::string& fk) const {
    return abs_root_ + "/" + fk;
}

//------------------------------------------------------------------------------

// True when the server's storage mount requires a signature for `key`: a
// url_prefix this server serves at, plus a secret to verify against. In that
// mode there is no resolvable unsigned URL.
bool LocalStorage::signing_enforced() const {
    return !config_.url_prefix.empty() && !config_.secret.empty();
}

// Build the unsigned object URL from a full key, irrespective of signing.
// public_url() and presigned_url() share this; public_url() additionally
// refuses to return it when signing is enforced.
std::string LocalStorage::object_url(const std::string& fk) const {
    // base_url set => absolute (e.g. a CDN origin): "<base_url><url_prefix>/<key>".
    // url_prefix (the serve path the mount strips back off) is appended for you, so
    // base_url is just the scheme://host -- no need to repeat the prefix in it.
    if (!config_.base_url.empty()) {
        return trim_trailing_slash(config_.base_url) + config_.url_prefix + "/"
             + uri_encode(fk, /*encode_slash=*/false);
    }
    // url_prefix set => relative same-origin URL: "<url_prefix>/<key>". The key
    // is NOT stored under url_prefix on disk; url_prefix is purely the serve path
    // the mount strips back off.
    if (!config_.url_prefix.empty()) {
        return trim_trailing_slash(config_.url_prefix) + "/" + uri_encode(fk, /*encode_slash=*/false);
    }
    // file:// URL. abs_root_ is "C:/..." on Windows and "/..." on POSIX; both
    // become "file:///<path>".
    const std::string path = disk_path(fk);
    return std::string("file://") + (path[0] == '/' ? "" : "/") + path;
}

std::string LocalStorage::public_url(const std::string& key) const {
    const std::string fk = full_key(key);
    // When the mount enforces signatures there is no resolvable unsigned URL, so
    // refuse to hand one out rather than ship a link that 403s. Callers want a
    // signed link in this mode: use presigned_url().
    if (signing_enforced()) {
        throw StorageError("LocalStorage: public_url() is unavailable when LOCAL_STORAGE_SECRET is set "
                           "(the server requires a signature); use presigned_url()");
    }
    return object_url(fk);
}

//------------------------------------------------------------------------------

std::string LocalStorage::presigned_url(const std::string& key, int expires_seconds) const {
    // Signing only buys anything when this server actually serves (and so can
    // verify) the object — i.e. a url_prefix is set — and there is a secret to
    // key the HMAC. Without that, hand back the plain URL: the file:// case has
    // no HTTP layer to check a signature, and an unset secret preserves the
    // historical "no signing on disk" behaviour.
    const std::string fk = full_key(key);
    if (!signing_enforced()) {
        return object_url(fk);
    }

    if (expires_seconds <= 0) expires_seconds = 3600;

    // Reuse a cached signed URL while it still has more than half its lifetime
    // left. Keying by (full_key, lifetime) keeps callers that ask for different
    // windows from colliding on one entry.
    const std::string cache_key = fk + "|" + std::to_string(expires_seconds);
    const std::time_t now = std::time(nullptr);
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = url_cache_.find(cache_key);
        if (it != url_cache_.end() && now < it->second.refresh_after) {
            return it->second.url;
        }
    }

    // Bind the signature to the object and its expiry so neither can be swapped
    // without invalidating it. The server recomputes the same HMAC to verify.
    const std::time_t expiry = now + expires_seconds;
    const std::string expiry_str = std::to_string(static_cast<long long>(expiry));
    const std::string to_sign = fk + "\n" + expiry_str;
    const std::array<unsigned char, 32> mac = hmac_sha256(config_.secret, to_sign);
    const std::string sig = hex_encode(mac.data(), mac.size());

    const std::string url = object_url(fk) + "?expires=" + expiry_str + "&sig=" + sig;

    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        url_cache_[cache_key] = SignedEntry{url, now + expires_seconds / 2};
    }
    return url;
}

//------------------------------------------------------------------------------

std::string LocalStorage::put_object(const std::string& key,
                                     const std::string& data,
                                     const std::string& /*content_type*/) {
    // Content-Type is not stored: the filesystem has no per-object metadata.
    // When served over HTTP the static-file route infers the type from the
    // extension, so keep meaningful extensions in the key.
    const std::string fk   = full_key(key);
    const std::string path = disk_path(fk);

    const std::size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        if (!make_dirs(path.substr(0, slash))) {
            throw StorageError("LocalStorage put_object: cannot create directory for '" + fk + "'");
        }
    }

    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) throw StorageError("LocalStorage put_object: cannot open '" + path + "' for writing");
    if (!data.empty()) out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.flush();
    if (!out) throw StorageError("LocalStorage put_object: write failed for '" + path + "'");
    out.close();

    return key;
}

//------------------------------------------------------------------------------

std::string LocalStorage::append_to_object(const std::string& key,
                                           const std::string& data,
                                           const std::string& /*content_type*/) {
    const std::string fk   = full_key(key);
    const std::string path = disk_path(fk);

    const std::size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        if (!make_dirs(path.substr(0, slash))) {
            throw StorageError("LocalStorage append_to_object: cannot create directory for '" + fk + "'");
        }
    }

    std::ofstream out(path.c_str(), std::ios::binary | std::ios::app);
    if (!out) throw StorageError("LocalStorage append_to_object: cannot open '" + path + "' for appending");
    if (!data.empty()) out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.flush();
    if (!out) throw StorageError("LocalStorage append_to_object: write failed for '" + path + "'");

    // Prefix-less key, matching put_object and the documented contract (the
    // prefix is an internal access detail, never part of a returned key).
    return std::string(key.data(), key.size());
}

//------------------------------------------------------------------------------

std::string LocalStorage::get_object(const std::string& key) {
    const std::string path = disk_path(full_key(key));
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) throw StorageError("LocalStorage get_object: '" + path + "' not found");

    std::string body;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size > 0) {
        body.resize(static_cast<std::size_t>(size));
        in.seekg(0, std::ios::beg);
        in.read(&body[0], size);
        body.resize(static_cast<std::size_t>(in.gcount()));
    }
    if (in.bad()) throw StorageError("LocalStorage get_object: read failed for '" + path + "'");
    return body;
}

//------------------------------------------------------------------------------

void LocalStorage::delete_object(const std::string& key) {
    const std::string path = disk_path(full_key(key));
    // Idempotent: a missing object is success, mirroring the S3/OSS backends.
    if (!is_regular_file(path)) return;
    if (std::remove(path.c_str()) != 0 && is_regular_file(path)) {
        throw StorageError("LocalStorage delete_object: cannot remove '" + path + "'");
    }
}

//------------------------------------------------------------------------------

std::vector<std::string> LocalStorage::list_objects(const std::string& prefix, std::size_t max_keys) {
    // The prefix keeps its trailing slash (meaningful for a prefix scan); only
    // leading slashes go. The configured key prefix is applied in front like
    // full_key does -- only when the caller's prefix doesn't already carry
    // it. Validate the combination with the same rules as object keys so a
    // crafted prefix ("..") can't walk outside the root.
    std::string p(prefix);
    while (!p.empty() && (p[0] == '/' || p[0] == '\\')) p.erase(0, 1);
    const std::string cfg  = trim_slashes(config_.prefix);
    const std::string cfgp = cfg.empty() ? std::string() : cfg + "/";
    const std::string fp = (!cfgp.empty() && p.compare(0, cfgp.size(), cfgp) == 0) ? p : cfgp + p;
    if (!fp.empty()) validate_key_segments(fp);   // throws StorageError on traversal

    // Walk from the deepest directory the prefix fully names, so a scan for
    // "a/b/file-" only touches a/b, not the whole root.
    const std::size_t slash = fp.find_last_of('/');
    const std::string sub   = slash == std::string::npos ? std::string() : fp.substr(0, slash);
    const std::string start = sub.empty() ? abs_root_ : abs_root_ + "/" + sub;

    std::vector<std::string> all;
    walk_files(start, sub, all);

    // Filter to the prefix; results are full keys (configured key prefix
    // included, matching S3/OSS), ordered lexicographically, capped.
    std::vector<std::string> out;
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (all[i].compare(0, fp.size(), fp) == 0) {
            out.push_back(all[i].substr(cfgp.size()));   // strip configured key prefix from results
            platform::log_debug("LocalStorage list_objects: found key '%s' matching prefix '%s'", all[i].c_str(), prefix.data());
        }
    }
    std::sort(out.begin(), out.end());
    if (out.size() > max_keys) out.resize(max_keys);
    return out;
}

//------------------------------------------------------------------------------

std::unique_ptr<Storage> LocalConfig::open() const {
    return std::unique_ptr<Storage>(new LocalStorage(*this));
}

//------------------------------------------------------------------------------

void LocalConfig::validate_signing_secret() const {
    // Only meaningful when this server signs/verifies: a url_prefix it serves at
    // plus a secret. A secret with no url_prefix is inert (never used to sign).
    if (url_prefix.empty() || secret.empty()) return;
    if (nonblank_len(secret) < kMinSecretChars) {
        throw StorageError("LocalStorage: LOCAL_STORAGE_SECRET must have at least "
                           "16 non-whitespace characters to sign URLs securely; "
                           "set a strong secret or unset it to disable signing");
    }
}

}}
