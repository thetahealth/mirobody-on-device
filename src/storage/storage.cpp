#include "storage/storage.hpp"

#include "storage/sign.hpp"   // hmac_sha1, base64url_encode

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <utility>

namespace mirobody { namespace storage {

// One extension <-> Content-Type table, read in both directions:
// resolve_content_type maps a filename extension to its type, suffix_for maps
// a type back to its extension. Both lookups take the FIRST matching row, so
// the canonical row for an alias group comes first: ".jpeg" resolves to
// image/jpeg but image/jpeg stores as ".jpg"; text/javascript stores as ".js"
// but ".js" resolves to application/javascript.
static const struct { const char* ext; const char* mime; } kTypes[] = {
    {".jpg",  "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".png",  "image/png"},
    {".webp", "image/webp"},
    {".gif",  "image/gif"},
    {".svg",  "image/svg+xml"},
    {".tif",  "image/tiff"},
    {".tiff", "image/tiff"},
    {".bmp",  "image/bmp"},
    {".ico",  "image/x-icon"},

    {".pdf",  "application/pdf"},
    {".json", "application/json"},
    {".xml",  "application/xml"},
    {".xml",  "text/xml"},
    {".js",   "application/javascript"},
    {".js",   "text/javascript"},
    {".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    {".xls",  "application/vnd.ms-excel"},
    {".zip",  "application/zip"},
    {".gz",   "application/gzip"},

    {".txt",  "text/plain"},
    {".html", "text/html"},
    {".htm",  "text/html"},
    {".css",  "text/css"},
    {".csv",  "text/csv"},
    {".md",   "text/markdown"},

    {".mp3",  "audio/mpeg"},
    {".wav",  "audio/wav"},
    {".aac",  "audio/aac"},
    {".flac", "audio/flac"},
    {".m4a",  "audio/mp4"},     // iOS voice memos / Android recordings (AAC in MPEG-4)
    {".m4a",  "audio/x-m4a"},
    {".caf",  "audio/x-caf"},   // iOS Core Audio Format
    {".amr",  "audio/amr"},     // Android voice recordings
    {".ogg",  "audio/ogg"},
    {".opus", "audio/opus"},

    {".mp4",  "video/mp4"},
    {".mov",  "video/quicktime"},   // iOS camera
    {".m4v",  "video/x-m4v"},
    {".3gp",  "video/3gpp"},        // Android camera / MMS; can be audio-only
    {".3gp",  "audio/3gpp"},
    {".3g2",  "video/3gpp2"},
    {".webm", "video/webm"},
    {".mkv",  "video/x-matroska"},
};
static const std::size_t kTypeCount = sizeof(kTypes) / sizeof(kTypes[0]);

std::string resolve_content_type(const std::string& filename,
                                 const std::string& declared) {
    const std::size_t dot = filename.find_last_of('.');
    if (dot != std::string::npos && dot + 1 < filename.size()) {
        std::string ext = filename.substr(dot);
        for (std::size_t i = 0; i < ext.size(); ++i) {
            if (ext[i] >= 'A' && ext[i] <= 'Z') ext[i] += 'a' - 'A';
        }
        for (std::size_t i = 0; i < kTypeCount; ++i) {
            if (ext == kTypes[i].ext) return std::string(kTypes[i].mime);
        }
    }
    if (!declared.empty()) return std::string(declared.data(), declared.size());
    return std::string("application/octet-stream");
}

// Filename suffix for a stored object, from its Content-Type (parameters like
// "; charset=..." are ignored). The suffix keeps keys meaningful and lets
// LocalStorage's HTTP mount -- which re-infers the served type from the
// extension -- answer with the type the object was stored as. An unknown type
// gets no suffix.
std::string suffix_for(const std::string& content_type) {
    std::string ct(content_type.data(), content_type.size());
    const std::size_t semi = ct.find(';');
    if (semi != std::string::npos) ct.erase(semi);
    while (!ct.empty() && (ct[ct.size() - 1] == ' ' || ct[ct.size() - 1] == '\t')) {
        ct.erase(ct.size() - 1);
    }
    for (std::size_t i = 0; i < kTypeCount; ++i) {
        if (ct == kTypes[i].mime) return std::string(kTypes[i].ext);
    }
    return std::string();
}

// One record of the <key>.meta sidecar: the durable note of how and when the
// object was stored, plus the caller's ObjectMeta (empty fields omitted).
// The sidecar is JSON Lines -- single JSON documents are hard to append, so
// every upload of these bytes APPENDS one record instead of rewriting: the
// first line is the original upload, the last the current one, and
// transcode/file rebuilds the per-user index from both. Deliberately NO user
// id: the key's hashed user segment exists so stored objects don't carry the
// sequential account id. Named ".meta" rather than ".json" because an upload
// key can end in ".json" (an application/json upload is <digest>.json), and
// a sidecar name an upload key can never take keeps the two distinguishable
// by shape alone.
std::string meta_json(const std::string& content_type,
                      std::size_t size, std::int64_t uploaded_at,
                      const ObjectMeta& meta) {
    rapidjson::Document d(rapidjson::kObjectType);
    rapidjson::Document::AllocatorType& a = d.GetAllocator();
    d.AddMember("content_type",
                rapidjson::Value(content_type.data(),
                                 static_cast<rapidjson::SizeType>(content_type.size()), a), a);
    d.AddMember("size", static_cast<uint64_t>(size), a);
    d.AddMember("uploaded_at", static_cast<int64_t>(uploaded_at), a);
    if (!meta.filename.empty()) {
        d.AddMember("filename",
                    rapidjson::Value(meta.filename.c_str(),
                                     static_cast<rapidjson::SizeType>(meta.filename.size()), a), a);
    }

    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    d.Accept(w);
    return std::string(sb.GetString(), sb.GetSize());
}

std::string Storage::put_user_object(std::int64_t user_id,
                                     const std::string& data,
                                     const std::string& content_type,
                                     const ObjectMeta& meta) {
    const std::string seed = object_key_seed();
    const std::array<unsigned char, 20> digest_user = hmac_sha1(seed, std::to_string(user_id));
    const std::array<unsigned char, 20> digest_data = hmac_sha1(seed, data);

    const std::string user_seg = base64url_encode(
        std::string(reinterpret_cast<const char*>(digest_user.data()), digest_user.size()));
    const std::string data_seg = base64url_encode(
        std::string(reinterpret_cast<const char*>(digest_data.data()), digest_data.size()));

    // Shard the per-user folder by the digest's first character (64-way) so one
    // user's objects spread across subdirectories instead of piling into a
    // single folder: LocalStorage is a real filesystem, and a directory with
    // very many entries scans slowly. transcode/file's key_shape_ok validates
    // this exact <user_seg>/<shard>/<digest> shape, so the two evolve together.
    const std::string key = user_seg + "/" + data_seg.substr(0, 1) + "/" + data_seg
                          + suffix_for(content_type);

    // put_object returns the prefix-less key -- the form recorded everywhere
    // downstream, so derive the sidecar from it too. The object body is
    // encrypted at rest when a cipher is configured; the KEY above is unchanged
    // (derived from the plaintext), so dedup holds.
    const std::string fk = put_object(key, encrypt_bytes(data), content_type);

    const std::string meta_key = get_meta_key(fk);
    const std::string line =
        meta_json(content_type, data.size(),
                  static_cast<std::int64_t>(std::time(nullptr)), meta) + "\n";
    if (file_encryption_enabled()) {
        // An encrypted .meta is one Fernet token over the whole JSON-Lines
        // body, so it can't be native-appended: read + decrypt the existing
        // records (if any), append this line, re-encrypt, and rewrite. Absent
        // is fine (first record); a real read error of an object that DOES
        // exist must not silently truncate it.
        std::string body;
        try {
            body = get_object(meta_key);
        } catch (const StorageError&) {
            if (object_exists(meta_key)) throw;
        }
        if (!body.empty()) body = decrypt_bytes(body);
        put_object(meta_key, encrypt_bytes(body + line), "application/x-ndjson");
    } else {
        append_to_object(meta_key, line, "application/x-ndjson");
    }
    return fk;
}

//------------------------------------------------------------------------------

void Storage::enable_file_encryption(std::vector<std::shared_ptr<encrypt::Fernet> > ciphers,
                                     std::string key_seed,
                                     std::string read_url_prefix,
                                     std::string read_url_base,
                                     std::string read_url_secret) {
    if (ciphers.empty()) return;
    file_ciphers_    = std::move(ciphers);
    key_hmac_key_    = std::move(key_seed);
    file_url_prefix_ = std::move(read_url_prefix);
    file_url_base_   = std::move(read_url_base);
    file_url_secret_ = std::move(read_url_secret);
}

// A fixed single-segment key (under the configured prefix via put/get):
// user-object keys are <27-char>/<27-char>, so this never collides with one.
static const char* kKeyMarker = ".file-encryption-check";

std::string Storage::read_key_marker() {
    try {
        return get_object(kKeyMarker);
    } catch (const StorageError&) {
        // Absent => "" (first run). But a transport error against a marker that
        // DOES exist must not be mistaken for absence (the caller would then
        // record over it and mask a real mismatch) -- probe to disambiguate.
        if (object_exists(kKeyMarker)) throw;
        return std::string();
    }
}

void Storage::write_key_marker(const std::string& content) {
    put_object(kKeyMarker, content, "text/plain");   // non-secret fingerprints
}

std::string Storage::encrypt_bytes(const std::string& plaintext) const {
    if (file_ciphers_.empty()) return std::string(plaintext.data(), plaintext.size());
    return file_ciphers_.back()->encrypt(plaintext);   // newest key encrypts
}

std::string Storage::decrypt_bytes(const std::string& token) const {
    if (file_ciphers_.empty()) return std::string(token.data(), token.size());
    // Try every configured key (newest first -- the common case): an object
    // written under an older key, kept in the list through a rotation, still
    // decrypts. Only when none matches is it a genuine failure.
    for (std::size_t i = file_ciphers_.size(); i-- > 0; ) {
        try {
            return file_ciphers_[i]->decrypt(token);
        } catch (const encrypt::FernetError&) {
            // wrong key for this object -- try the next
        }
    }
    throw StorageError("Storage: object decryption failed (no configured key matched)");
}

std::string Storage::get_object_decrypted(const std::string& key) {
    const std::string raw = get_object(key);
    if (file_ciphers_.empty()) return raw;
    return decrypt_bytes(raw);
}

std::string Storage::put_object_encrypted(const std::string& key,
                                          const std::string& data,
                                          const std::string& content_type) {
    if (file_ciphers_.empty()) return put_object(key, data, content_type);
    return put_object(key, encrypt_bytes(data), content_type);
}

//------------------------------------------------------------------------------

namespace {

// Parse a marker body into the seed fingerprint and the set of key
// fingerprints. Format is one "field=value" per line (blank lines and stray
// '\r' tolerated): one `seed=` line, then a `key=` line per known key.
void parse_key_marker(const std::string& marker, std::string& seed,
                      std::vector<std::string>& keys) {
    std::size_t start = 0;
    while (start <= marker.size()) {
        std::size_t nl = marker.find('\n', start);
        std::string line = marker.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t eq = line.find('=');
        if (eq != std::string::npos) {
            const std::string k = line.substr(0, eq);
            const std::string v = line.substr(eq + 1);
            if (k == "seed") seed = v;
            else if (k == "key" && !v.empty()) keys.push_back(v);
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
}

std::string build_key_marker(const std::string& seed, const std::vector<std::string>& keys) {
    std::string out = "seed=" + seed + "\n";
    for (std::size_t i = 0; i < keys.size(); ++i) out += "key=" + keys[i] + "\n";
    return out;
}

bool contains_str(const std::vector<std::string>& v, const std::string& s) {
    for (std::size_t i = 0; i < v.size(); ++i) if (v[i] == s) return true;
    return false;
}

}  // namespace

KeyMarkerCheck reconcile_key_marker(const std::string& marker,
                                    const std::string& seed_fingerprint,
                                    const std::vector<std::string>& key_fingerprints) {
    KeyMarkerCheck r;

    if (marker.empty()) {   // first run: record what we have
        r.updated_marker = build_key_marker(seed_fingerprint, key_fingerprints);
        return r;
    }

    std::string stored_seed;
    std::vector<std::string> stored_keys;
    parse_key_marker(marker, stored_seed, stored_keys);

    if (stored_seed != seed_fingerprint) {
        r.ok = false;
        r.reason = "FILE_KEY_SEED does not match the value existing uploads were stored under; "
                   "every object's path is derived from it, so a change orphans them all. Restore "
                   "the original FILE_KEY_SEED. (It must never change once data exists.)";
        return r;
    }

    // At least one configured key must be a key the store has seen, or the whole
    // FILE_ENCRYPTION_KEY list was replaced with unrelated keys -- refuse.
    bool intersects = false;
    for (std::size_t i = 0; i < key_fingerprints.size(); ++i) {
        if (contains_str(stored_keys, key_fingerprints[i])) { intersects = true; break; }
    }
    if (!intersects) {
        r.ok = false;
        r.reason = "none of the configured FILE_ENCRYPTION_KEY entries matches a key existing "
                   "uploads were encrypted with; refusing to start to avoid leaving them "
                   "unreadable. Restore at least one previous key (rotate by appending a new key, "
                   "not replacing the list).";
        return r;
    }

    // Grow the known-key set with any newly-appended keys (union), so a later
    // drop of an older key is still recognized as a known rotation.
    std::vector<std::string> merged = stored_keys;
    bool grew = false;
    for (std::size_t i = 0; i < key_fingerprints.size(); ++i) {
        if (!contains_str(merged, key_fingerprints[i])) { merged.push_back(key_fingerprints[i]); grew = true; }
    }
    if (grew) r.updated_marker = build_key_marker(stored_seed, merged);
    return r;
}

FileEncryptionResult configure_file_encryption(Storage& storage,
                                               const std::vector<std::string>& keys,
                                               const std::string& seed,
                                               const std::string& url_prefix,
                                               const std::string& url_base) {
    FileEncryptionResult res;
    if (keys.empty()) return res;   // encryption off: ok, not enabled

    if (seed.empty()) {
        res.ok = false;
        res.error = "FILE_ENCRYPTION_KEY is set but FILE_KEY_SEED is not; encryption needs a "
                    "stable, separate seed for object-key paths. Set FILE_KEY_SEED to a strong "
                    "secret and never change it once uploads exist.";
        return res;
    }

    // Cipher list: last key encrypts, all decrypt. A bad key throws FernetError.
    std::vector<std::shared_ptr<encrypt::Fernet> > ciphers;
    std::vector<std::string> key_fingerprints;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        ciphers.push_back(std::make_shared<encrypt::Fernet>(keys[i]));
        const std::array<unsigned char, 32> fp = hmac_sha256(keys[i], "mirobody-file-key-fingerprint");
        key_fingerprints.push_back(hex_encode(fp.data(), fp.size()));
    }
    // Two secrets + the seed fingerprint, all derived from FILE_KEY_SEED with
    // distinct labels (single source of these labels, so the server and the C
    // API agree).
    const std::array<unsigned char, 32> seed_mac = hmac_sha256(seed, "mirobody-object-key");
    const std::string key_seed = hex_encode(seed_mac.data(), seed_mac.size());
    const std::array<unsigned char, 32> url_mac = hmac_sha256(seed, "mirobody-file-read-url");
    res.url_secret = hex_encode(url_mac.data(), url_mac.size());
    const std::array<unsigned char, 32> seed_fp_mac = hmac_sha256(seed, "mirobody-seed-fingerprint");
    const std::string seed_fp = hex_encode(seed_fp_mac.data(), seed_fp_mac.size());

    storage.enable_file_encryption(ciphers, key_seed, url_prefix, url_base, res.url_secret);

    const KeyMarkerCheck chk =
        reconcile_key_marker(storage.read_key_marker(), seed_fp, key_fingerprints);
    if (!chk.ok) {
        res.ok = false;
        res.error = chk.reason;
        return res;
    }
    if (!chk.updated_marker.empty()) storage.write_key_marker(chk.updated_marker);

    res.enabled = true;
    return res;
}

std::string Storage::signed_read_url(const std::string& key, int expires_seconds) const {
    if (!file_encryption_enabled()) return presigned_url(key, expires_seconds);
    if (expires_seconds <= 0) expires_seconds = 3600;

    // Sign over the FULL key and absolute expiry, exactly the tuple the
    // router's decrypting mount recomputes; the mount then fetches and
    // decrypts the bytes server-side. `file_url_base_` empty => a relative
    // same-origin URL ("/files/<key>?..."); set => absolute (e.g. a gateway
    // origin), with this server still serving the bytes.
    const std::string fk = full_key(key);
    const std::time_t expiry = std::time(nullptr) + expires_seconds;
    const std::string expiry_str = std::to_string(static_cast<long long>(expiry));
    const std::string to_sign = fk + "\n" + expiry_str;
    const std::array<unsigned char, 32> mac = hmac_sha256(file_url_secret_, to_sign);
    const std::string sig = hex_encode(mac.data(), mac.size());

    return file_url_base_ + file_url_prefix_ + "/" + uri_encode(fk, /*encode_slash=*/false)
         + "?expires=" + expiry_str + "&sig=" + sig;
}

std::string get_meta_key(const std::string& key) { return key + ".meta"; }
std::string get_trans_key(const std::string& key) { return key + ".trans"; }

bool is_meta_key(const std::string& key) {
    static const char kSuffix[] = ".meta";
    const std::size_t n = sizeof(kSuffix) - 1;
    return key.size() >= n && key.compare(key.size() - n, n, kSuffix) == 0;
}
bool is_trans_key(const std::string& key) {
    static const char kSuffix[] = ".trans";
    const std::size_t n = sizeof(kSuffix) - 1;
    return key.size() >= n && key.compare(key.size() - n, n, kSuffix) == 0;
}

std::string strip_sidecar_suffix(const std::string& key) {
    if (is_meta_key(key)) {
        return key.substr(0, key.size() - 5);
    }
    if (is_trans_key(key)) {
        return key.substr(0, key.size() - 6);
    }
    return key;
}

bool Storage::object_exists(const std::string& key) {
    // Normalize both sides through full_key (idempotent) so the compare holds
    // regardless of whether list_objects / the probe key carry the prefix --
    // the exact key sorts first among the keys it prefixes.
    const std::vector<std::string> hits = list_objects(key, 1);
    return !hits.empty() && full_key(hits[0]) == full_key(key);
}

std::string Storage::append_to_object(const std::string& key,
                                      const std::string& data,
                                      const std::string& content_type) {
    std::string body;
    try {
        body = get_object(key);
    } catch (const StorageError&) {
        // Absent is fine (append-to-empty creates the object), but a failed
        // read of an object that DOES exist must not fall through -- the put
        // below would truncate it to just `data`. get_object can't tell the
        // two apart, so probe.
        if (object_exists(key)) throw;
    }
    body.append(data.data(), data.size());
    return put_object(key, body, content_type);
}

std::vector<std::string> Storage::list_user_objects(std::int64_t user_id,
                                                    std::size_t max_keys) {
    const std::array<unsigned char, 20> digest_user = hmac_sha1(object_key_seed(), std::to_string(user_id));

    const std::string prefix =
            base64url_encode(
                std::string(reinterpret_cast<const char*>(digest_user.data()), digest_user.size())
            )
            + "/";

    return list_objects(prefix, max_keys);
}

bool Storage::belongs_to_user(const std::string& key, std::int64_t user_id) {
    const std::array<unsigned char, 20> digest_user = hmac_sha1(object_key_seed(), std::to_string(user_id));
    const std::string user_seg = base64url_encode( 
        std::string(reinterpret_cast<const char*>(digest_user.data()), digest_user.size())
    ) + "/";
    return std::string(key).rfind(user_seg, 0) == 0;
}

}}
