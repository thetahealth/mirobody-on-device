#pragma once

// Object-storage abstraction: the interface the rest of the core stores uploads,
// charts and extracted text through. One backend, LocalStorage
// (src/storage/local_storage.*): on a phone every object is a file in the app
// sandbox. Kept as a virtual interface so a test can substitute its own. The
// cloud object stores (S3, OSS, Azure Blob) belong to the server in the main
// mirobody repo.

#include "compat/cxx11.hpp"
#include "config/fernet.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace mirobody { namespace storage {

static const char *default_prefix = "mirobody";

class Storage;

//------------------------------------------------------------------------------

// Thrown on any non-success outcome: transport failure, a non-2xx response
// other than the 404 that delete_object() tolerates, or misconfiguration.
class StorageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

//------------------------------------------------------------------------------
// Backend configuration structs
//------------------------------------------------------------------------------
//
// Populated by the lazy getter on mirobody::Config (cfg.local_storage()).
// open() constructs the backend; callers hold the returned Storage through the
// interface and never name the concrete type.

// Local filesystem. Objects are written as files under `root`; the key becomes
// the relative path beneath it. Loaded from LOCAL_STORAGE_DIR /
// LOCAL_STORAGE_PREFIX / LOCAL_STORAGE_URL_PREFIX / LOCAL_STORAGE_BASE_URL /
// LOCAL_STORAGE_SECRET (see Config::local_storage).
//
// `prefix` (LOCAL_STORAGE_PREFIX) is the object-key prefix, same contract as
// S3_PREFIX / ALI_OSS_PREFIX: prepended to every key internally, so callers
// pass prefix-agnostic keys and objects land under `root`/<prefix>/.
//
// `url_prefix` (LOCAL_STORAGE_URL_PREFIX) is the URL path THIS server serves
// objects at — a URL concern only, NOT part of the stored path. With url_prefix
// "/files", put_object("chart.png") stores `root`/chart.png (flat under root,
// or under the key prefix when one is set) and the router mounts it at
// "/files/chart.png", stripping the prefix back off to find the file
// (Router::set_storage_mount). `root` need NOT live under HTTP_ROOT, so uploads
// stay out of the web root instead of being exposed as ordinary static assets.
// Empty url_prefix => this server does not serve them.
//
// `base_url` (LOCAL_STORAGE_BASE_URL) is the scheme://host public_url() /
// presigned_url() build the public URL on. Leave it empty for a relative same-origin
// URL ("/files/chart.png"); set it to an absolute origin (e.g. a CDN host) for
// absolute URLs ("https://cdn/files/chart.png"), with this server still serving
// the bytes. url_prefix is appended automatically, so base_url is the HOST ONLY —
// do not repeat the prefix in it (a trailing "/files" would double to
// "/files/files"). With neither url_prefix nor base_url, the URL is file://.
//
// presigned_url() appends "?expires=<unix>&sig=<hmac>" — an HMAC-SHA256 keyed
// by `secret` — and the mount verifies it (recompute + expiry check) before
// serving, rejecting a missing/expired/forged signature with 403. The signature
// is over the object key, so it survives a CDN forwarding the query to origin.
// With no `secret` (or no `url_prefix`) there is nothing to sign against, so
// presigned_url() == public_url() and its expiry is ignored.
//
// When signing IS enforced (both `secret` and `url_prefix` set), no unsigned URL
// resolves, so public_url() throws; use presigned_url().
struct LocalConfig {
    std::string root;        // LOCAL_STORAGE_DIR — base directory for objects
    std::string prefix;      // LOCAL_STORAGE_PREFIX — key prefix prepended to every object (optional)
    std::string url_prefix;  // LOCAL_STORAGE_URL_PREFIX — URL serve path (optional; not stored on disk)
    std::string base_url;    // LOCAL_STORAGE_BASE_URL — public URL prefix; absolute for CDN (optional)
    std::string secret;      // LOCAL_STORAGE_SECRET — HMAC key for presigned_url() (optional)

    // Build a LocalStorage backend, creating `root` if needed. Throws
    // StorageError if `root` is empty or cannot be created. Defined in
    // local_storage.cpp.
    std::unique_ptr<Storage> open() const;

    // Throws StorageError when signing is configured (url_prefix + secret both
    // set) but the secret is too weak to be a real HMAC key (fewer than 16
    // non-whitespace characters) — a blank/trivial key would make signatures
    // forgeable. No-op otherwise. Call before relying on the secret for signing
    // or verification (LocalStorage's ctor and the server's mount setup do).
    // Defined in local_storage.cpp.
    void validate_signing_secret() const;

    bool configured() const { return !root.empty(); }
};

//------------------------------------------------------------------------------
// Storage
//------------------------------------------------------------------------------

// Caller-supplied metadata recorded in the <key>.meta sidecar that
// put_user_object writes, alongside the fields it derives itself
// (content_type, size, uploaded_at). Every field is optional; empty fields
// are omitted from the sidecar. Extend with new fields as uploads carry more
// context.
struct ObjectMeta {
    std::string filename;   // the client's original filename
};

//------------------------------------------------------------------------------

// Object-store operations. Keys RETURNED by the API (put_object,
// append_to_object, put_user_object, list_objects, list_user_objects) are
// PREFIX-LESS: the configured prefix (S3_PREFIX / ALI_OSS_PREFIX /
// LOCAL_STORAGE_PREFIX) is an internal storage-access detail, applied only
// when a method actually reads or writes the backend (via full_key()), and
// never part of a returned key. So a returned key is a portable handle that
// stays valid across a prefix change once the objects are physically
// relocated, and indexes / URLs / DB columns that hold it don't bake in this
// deployment's prefix. Keys PASSED IN may still carry the prefix or omit it
// -- full_key() prepends it only when absent (idempotent), so keys recorded
// before a prefix was configured keep resolving. Implementations are safe to
// use from one thread at a time; share one Storage per worker or serialize
// access.
class Storage {
public:
    virtual ~Storage() = default;

    // The backend's bucket name; "" for the local filesystem. Stable per
    // deployment -- put_user_object keys both of its hashed segments with
    // it, so the key scheme requires deployment config, not just this
    // (public) source.
    virtual std::string bucket_name() const = 0;

    // `key` with the configured prefix applied -- the bucket-relative form
    // every returned key has. Idempotent: a key already carrying the prefix
    // (any API-returned key) passes through unchanged, which is what lets
    // the read methods accept both forms. The check is by first segment(s),
    // so a caller-chosen key whose leading segment equals the prefix is
    // treated as already prefixed; put_user_object's hashed segments cannot
    // collide with a prefix. LocalStorage additionally validates the result
    // stays under its root (throws StorageError).
    virtual std::string full_key(const std::string& key) const = 0;

    // Store an uploaded file under its user-scoped key and return that key
    // -- the caller's durable handle for indexing, sidecars, and minting
    // URLs (presigned_url / public_url). The returned key is prefix-less
    // (<user seg>/<shard>/<digest>[.<suffix>]; the configured prefix is applied
    // only at backend access, never returned; <shard> is the digest's first
    // char, a 64-way per-user folder fan-out so no one directory holds all of a
    // user's objects -- LocalStorage is a real filesystem); the two hashed
    // segments are
    // unpadded-base64url HMAC-SHA1 keyed by the bucket name (27 chars of
    // [A-Za-z0-9_-] each): the first over the decimal user id -- the
    // per-user prefix all of a user's objects and metadata sidecars share,
    // hashed so URLs don't leak the sequential account id -- and the second
    // over the file bytes, so repeat uploads of identical bytes land on the
    // same object while keys stay unguessable without the bytes and the
    // deployment config. The suffix is a filename extension derived from
    // `content_type` (e.g. ".pdf"; none when the type is unknown), so
    // LocalStorage's extension-driven HTTP mount serves the object as the
    // type it was stored with. Also appends one record to the <key>.meta
    // sidecar -- JSON Lines, one {"content_type", "size", "uploaded_at"
    // (unix seconds)} line per upload of these bytes, plus whatever `meta`
    // carries (e.g. the original filename); deliberately no user id, which
    // stored objects must not leak. The sidecar is the durable per-file
    // record transcode/file rebuilds the upload index from (first record =
    // original upload, last = current). `user_id` must be > 0.
    // transcode/file re-derives the user segment and validates this exact
    // key shape (key_shape_ok), so the two must evolve together. Throws
    // StorageError on failure. Defined in storage.cpp.
    std::string put_user_object(std::int64_t user_id,
                                const std::string& data,
                                const std::string& content_type = "application/octet-stream",
                                const ObjectMeta& meta = ObjectMeta());

    bool belongs_to_user(const std::string& key, std::int64_t user_id);

    // Keys of this user's stored objects (uploads plus their .meta /
    // .trans sidecars), in lexicographic order: a bounded list_objects
    // scan of the user's hashed key prefix (the first segment of every
    // put_user_object key). At most `max_keys` keys. Throws StorageError on
    // failure. Defined in storage.cpp.
    std::vector<std::string> list_user_objects(std::int64_t user_id,
                                               std::size_t max_keys = 1000);

    // Upload `data` under `key` with the given Content-Type. Returns the
    // prefix-less key (the configured prefix is applied internally at write,
    // not returned); URLs are minted separately via presigned_url() /
    // public_url(). Throws StorageError on failure.
    virtual std::string put_object(const std::string& key,
                                   const std::string& data,
                                   const std::string& content_type = "application/octet-stream") = 0;

    // True when an object exists at exactly `key`. A bounded prefix-listing
    // probe -- the exact key sorts first among the keys it prefixes -- so no
    // body transfer. Throws StorageError on failure. Defined in storage.cpp.
    bool object_exists(const std::string& key);

    // Append `data` to the object at `key`, creating it when absent, and
    // return the prefix-less key. The base implementation is a read-modify-write (get +
    // put of the concatenation): not atomic under concurrent writers, and
    // the whole object rides through memory -- meant for logs and
    // incremental records, not large objects. Backends with a native append
    // override it: LocalStorage appends to the file directly.
    // `content_type` applies to the (re)written object. Throws StorageError
    // on failure (including a failed read of an object that does exist --
    // never truncates). Defined in storage.cpp.
    virtual std::string append_to_object(const std::string& key,
                                         const std::string& data,
                                         const std::string& content_type = "application/octet-stream");

    // Download the object at `key`. Throws StorageError if it is absent (404)
    // or on any transport / non-2xx error.
    virtual std::string get_object(const std::string& key) = 0;

    // Delete the object at `key`. Idempotent: a missing object is not an error
    // (both S3 and OSS return success for DELETE of a nonexistent key).
    virtual void delete_object(const std::string& key) = 0;

    // Keys of the objects whose key starts with `prefix`, in lexicographic
    // order. Like every other method, `prefix` may carry the configured
    // bucket prefix or omit it (applied internally when absent); the
    // returned keys are prefix-less (the configured prefix is stripped).
    // Pagination is handled internally; at most `max_keys` keys are returned,
    // so this is a bounded scan, not an unbounded bucket walk. Throws
    // StorageError on failure.
    virtual std::vector<std::string> list_objects(const std::string& prefix,
                                                  std::size_t max_keys = 1000) = 0;

    // A signed GET URL for `key` that grants temporary read access without
    // credentials, valid for `expires_seconds` from now. Computed locally — no
    // network call. Use to hand a private object to a browser or another
    // service for a bounded window.
    virtual std::string presigned_url(const std::string& key,
                                      int expires_seconds = 3600) const = 0;

    // The unsigned public URL for `key` (via the CDN/custom domain when set).
    // Only resolvable if the object or bucket is publicly readable.
    virtual std::string public_url(const std::string& key) const = 0;

    //--------------------------------------------------------------------------
    // Encryption at rest (optional)
    //--------------------------------------------------------------------------
    //
    // When a file-encryption key is configured (FILE_ENCRYPTION_KEY), user
    // uploads and their .meta / .trans sidecars are stored Fernet-encrypted:
    // a leaked object-store credential then yields ciphertext, not file
    // contents, because the Fernet key lives in app config, separate from the
    // S3/OSS secret. The object KEY is still derived from the plaintext bytes
    // (content-addressed dedup is preserved); only the stored body is
    // encrypted. Disabled (every method below is passthrough) when no cipher is
    // set. Configured once at startup, after open(), via the server.

    // Turn on encryption. `ciphers` is the FILE_ENCRYPTION_KEY list: the LAST
    // entry encrypts new writes, and ALL entries are tried on read (so objects
    // written under an older key keep decrypting through a key rotation).
    // `key_seed` is the stable HMAC key object-key derivation switches to (see
    // object_key_seed), derived from FILE_KEY_SEED -- kept separate from the
    // ciphers so rotating them leaves paths unchanged. read_url_prefix /
    // read_url_base / read_url_secret parameterize signed_read_url() -- the
    // app-served, in-memory-decrypted read endpoint that replaces bucket-direct
    // / local-mount URLs while encryption is on (those would hand a client
    // ciphertext). An empty `ciphers` is a no-op (encryption stays off).
    void enable_file_encryption(std::vector<std::shared_ptr<encrypt::Fernet> > ciphers,
                                std::string key_seed,
                                std::string read_url_prefix,
                                std::string read_url_base,
                                std::string read_url_secret);

    bool file_encryption_enabled() const { return !file_ciphers_.empty(); }

    // Raw read/write of the key-fingerprint marker object
    // (".file-encryption-check" under the configured prefix) -- the durable
    // record reconcile_key_marker() compares the configured keys against so an
    // accidental key/seed change fails loudly at startup instead of silently
    // orphaning uploads. read returns "" when the marker is absent (throwing
    // only on a genuine transport error); write stores `content` as plaintext
    // (it is non-secret HMAC fingerprints, not file content). Defined in
    // storage.cpp.
    std::string read_key_marker();
    void        write_key_marker(const std::string& content);

    // The HMAC key that put_user_object / list_user_objects / belongs_to_user
    // (and transcode/file's matching re-derivation) key the object-key hashes
    // with. Normally the bucket name -- low-entropy and known to anyone holding
    // the storage credential, but that is fine when objects are plaintext, and
    // it keeps keys derivable by another stack sharing the bucket. When file
    // encryption is on it is instead a stable secret derived from FILE_KEY_SEED,
    // so a leaked storage credential alone can no longer recompute a user's
    // object keys -- closing the confirmation-of-file gap that a public,
    // credential-derivable key seed (bucket name, region, ...) leaves open.
    // The digest length is unchanged (still HMAC-SHA1), so the key shape and
    // its validators are unaffected.
    std::string object_key_seed() const {
        return file_encryption_enabled() ? key_hmac_key_ : bucket_name();
    }

    // get_object() then decrypt when encryption is on (plain get_object()
    // otherwise). The read seam for every user-content fetch -- upload bytes,
    // .meta, .trans. Throws StorageError if the object is absent or fails to
    // decrypt (a caller that treats absence as benign catches it, as today).
    std::string get_object_decrypted(const std::string& key);

    // put_object() with the bytes encrypted first when encryption is on (plain
    // put_object() otherwise). The write seam for a sidecar stored next to an
    // upload (.trans); put_user_object encrypts the object bytes and .meta
    // itself. Returns the prefix-less key (same as put_object).
    std::string put_object_encrypted(const std::string& key,
                                     const std::string& data,
                                     const std::string& content_type = "application/octet-stream");

    // A signed, time-limited read URL for `key`, for handing to a client (chat
    // upload events, the per-user file index). With encryption ON the bytes
    // must flow back through this server decrypted, so this mints a URL to the
    // app's own file mount, HMAC-signed with the read-URL secret and verified
    // by the router's decrypting mount. With encryption OFF it is exactly
    // presigned_url() (bucket-direct / local mount), preserving today's
    // behaviour.
    std::string signed_read_url(const std::string& key, int expires_seconds = 3600) const;

protected:
    // Encrypt / decrypt a blob with the configured cipher; passthrough (return
    // input verbatim) when none is set. decrypt_bytes wraps a Fernet failure as
    // StorageError so callers see one error type.
    std::string encrypt_bytes(const std::string& plaintext) const;
    std::string decrypt_bytes(const std::string& token) const;

private:
    std::vector<std::shared_ptr<encrypt::Fernet> > file_ciphers_;  // empty => off; back() encrypts, all decrypt
    std::string key_hmac_key_;      // object-key HMAC seed while encrypted (see object_key_seed)
    std::string file_url_prefix_;   // app file-mount path, e.g. "/files"
    std::string file_url_base_;     // absolute origin for read URLs; empty => same-origin (relative)
    std::string file_url_secret_;   // HMAC key for signed_read_url() (derived from the Fernet key)
};

//------------------------------------------------------------------------------

// Outcome of reconciling the configured file-encryption keys / seed against the
// recorded marker. `ok == false` means the caller must refuse to start, with
// `reason` the operator-facing explanation. `updated_marker`, when non-empty,
// is new marker content to write back (first run, or the known-key set grew).
struct KeyMarkerCheck {
    bool        ok = true;
    std::string reason;
    std::string updated_marker;
};

// Decide whether the configured keys are consistent with what existing uploads
// were encrypted under. `marker` is the stored marker content ("" if absent);
// `seed_fingerprint` is a non-secret HMAC of FILE_KEY_SEED; `key_fingerprints`
// are non-secret HMACs of each configured FILE_ENCRYPTION_KEY entry. Rules:
//   - First run (empty marker): ok, record the seed + all key fingerprints.
//   - Seed fingerprint differs from the record: NOT ok (FILE_KEY_SEED changed
//     -- every object's path would shift; refuse).
//   - None of the configured key fingerprints appears in the recorded set:
//     NOT ok (the whole key list was replaced with unrelated keys; refuse).
//   - Otherwise ok; if the configured keys add any new fingerprint, the record
//     grows (union) so a later drop of an old key is still recognized.
// A dropped key that still has objects encrypted under it CANNOT be detected
// here (it needs a full scan), so removing a key remains an operator
// responsibility -- re-write or expire its objects first. Defined in storage.cpp.
KeyMarkerCheck reconcile_key_marker(const std::string& marker,
                                    const std::string& seed_fingerprint,
                                    const std::vector<std::string>& key_fingerprints);

//------------------------------------------------------------------------------

// Outcome of configure_file_encryption.
struct FileEncryptionResult {
    bool        ok = true;        // false => caller must refuse to proceed
    bool        enabled = false;  // true => encryption was configured on `storage`
    std::string url_secret;       // HMAC key for the read-URL mount (when enabled)
    std::string error;            // operator-facing message when !ok
};

// Configure encryption-at-rest on `storage` from the parsed config values, and
// run the key/seed fingerprint guard. The single place the key derivation
// (object-key seed, read-URL secret, fingerprints) and the startup guard live,
// shared by the server and the C API so they can't drift:
//   - `keys` empty  => no-op success (encryption stays off; ok, !enabled).
//   - `keys` set but `seed` empty => NOT ok (a stable seed is required).
//   - otherwise build the cipher list (last encrypts, all decrypt), derive the
//     secrets from `seed`, enable_file_encryption(), and reconcile_key_marker()
//     against the recorded marker -- NOT ok on a changed seed / replaced key
//     list, else ok+enabled (recording or growing the marker as needed).
// `url_prefix` / `url_base` parameterize signed_read_url(); pass them through
// from config. A bad Fernet key throws encrypt::FernetError. Defined in
// storage.cpp.
FileEncryptionResult configure_file_encryption(Storage& storage,
                                               const std::vector<std::string>& keys,
                                               const std::string& seed,
                                               const std::string& url_prefix,
                                               const std::string& url_base);

//------------------------------------------------------------------------------

// The Content-Type to store an upload as, resolved extension-first: a
// filename extension known to the shared extension<->type table wins (the
// server-derived answer -- and the same table put_user_object derives the key
// suffix from, so the stored suffix and the type always agree), then the
// client-declared type verbatim, then "application/octet-stream". The
// extension match is case-insensitive. Defined in storage.cpp.
std::string resolve_content_type(const std::string& filename,
                                 const std::string& declared);

//------------------------------------------------------------------------------

// The sidecar keys derived from an object key, and their recognizers -- the
// single definition of the ".meta" (put_user_object's metadata record)
// and ".trans" (extracted text, chat upload path) naming, so writers and
// scanners (e.g. transcode/file's index rebuild) can't drift. Defined in
// storage.cpp.
std::string get_meta_key(const std::string& key);   // <key>.meta
std::string get_trans_key(const std::string& key);   // <key>.trans
bool is_meta_key(const std::string& key);
bool is_trans_key(const std::string& key);
std::string strip_sidecar_suffix(const std::string& key);
}}
