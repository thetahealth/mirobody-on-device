#include "storage/local_storage.hpp"
#include "storage/sign.hpp"
#include "storage/storage.hpp"

#include "config/fernet.hpp"

#include <memory>

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace mirobody::storage;

namespace {

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}

//------------------------------------------------------------------------------

TEST_CASE("resolve_content_type is extension-first with declared fallback", "[storage]") {
    // A known extension wins, case-insensitively, even over a declared type.
    REQUIRE(resolve_content_type("photo.JPG", "") == "image/jpeg");
    REQUIRE(resolve_content_type("report.pdf", "text/plain") == "application/pdf");
    REQUIRE(resolve_content_type("song.flac", "") == "audio/flac");

    // Unknown extension: the declared type passes through verbatim.
    REQUIRE(resolve_content_type("data.parquet", "application/x-parquet") ==
            "application/x-parquet");
    REQUIRE(resolve_content_type("notes", "text/plain") == "text/plain");

    // Nothing to go on -> octet-stream.
    REQUIRE(resolve_content_type("data.parquet", "") == "application/octet-stream");
    REQUIRE(resolve_content_type("blob", "") == "application/octet-stream");
}

//------------------------------------------------------------------------------

namespace {

LocalConfig sample_local() {
    LocalConfig c;
    c.root       = "_local/test/storage";   // created under the cwd (gitignored _local/)
    c.url_prefix = "/files";   // URL serve path; relative public_url prefix
    return c;
}

bool file_on_disk(const std::string& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    return f.good();
}

// Recursively delete `path` (file or whole directory tree); no-op if absent.
// The test uses the portable directory API so it runs on every host profile.
void remove_tree(const std::string& path) {
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((path + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::string name = fd.cFileName;
            if (name == "." || name == "..") continue;
            const std::string child = path + "\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(child);
            else DeleteFileA(child.c_str());
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(path.c_str());
#else
    if (DIR* d = ::opendir(path.c_str())) {
        while (struct dirent* e = ::readdir(d)) {
            const std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            const std::string child = path + "/" + name;
            struct stat st;
            if (::stat(child.c_str(), &st) == 0 && (st.st_mode & S_IFDIR)) remove_tree(child);
            else ::unlink(child.c_str());
        }
        ::closedir(d);
    }
    ::rmdir(path.c_str());
#endif
}

// Tear down the storage tests' scratch tree once the run finishes, so a test
// run leaves nothing behind. _local/ is gitignored; we only own _local/test.
struct LocalStorageScratchCleanup : Catch::EventListenerBase {
    using Catch::EventListenerBase::EventListenerBase;
    void testRunEnded(Catch::TestRunStats const&) override { remove_tree("_local/test"); }
};
CATCH_REGISTER_LISTENER(LocalStorageScratchCleanup)

}

TEST_CASE("LocalConfig::open requires a root directory", "[storage][local]") {
    LocalConfig c;
    REQUIRE_THROWS_AS(c.open(), StorageError);
    c.root = "_local/test/storage";
    REQUIRE_NOTHROW(c.open());
}

//------------------------------------------------------------------------------

TEST_CASE("LocalStorage rejects a weak signing secret", "[storage][local]") {
    LocalConfig c = sample_local();   // url_prefix "/files" -> signing in play

    // Blank, whitespace-only, and too-short secrets are forgeable keys: reject.
    c.secret = "short";
    REQUIRE_THROWS_AS(c.open(), StorageError);
    c.secret = "                   ";   // whitespace only -> 0 real chars
    REQUIRE_THROWS_AS(c.open(), StorageError);

    // A strong secret is accepted.
    c.secret = "a-sufficiently-long-secret";
    REQUIRE_NOTHROW(c.open());

    // The secret is inert without a url_prefix (no signing), so it isn't checked.
    c.url_prefix.clear();
    c.secret = "x";
    REQUIRE_NOTHROW(c.open());
}

//------------------------------------------------------------------------------

TEST_CASE("LocalStorage round-trips text and binary objects", "[storage][local]") {
    LocalStorage ls(sample_local());

    // Text object in a nested key. put_object returns the storage key; the
    // URL is minted separately (relative, rooted at url_prefix, since there
    // is no base_url).
    const std::string key = ls.put_object("a/b/hello.txt", "hello world", "text/plain");
    REQUIRE(key == "a/b/hello.txt");
    REQUIRE(ls.get_object("a/b/hello.txt") == "hello world");
    REQUIRE(ls.presigned_url("a/b/hello.txt", 60) == "/files/a/b/hello.txt");   // no secret -> no signing

    // Binary payload with embedded NULs survives the round trip.
    const std::string binary("\x00\x01\x02\x00\xff", 5);
    ls.put_object("blob.bin", binary, "application/octet-stream");
    REQUIRE(ls.get_object("blob.bin") == binary);

    // Delete is idempotent: removing twice is not an error, and a get after
    // delete fails.
    ls.delete_object("a/b/hello.txt");
    REQUIRE_THROWS_AS(ls.get_object("a/b/hello.txt"), StorageError);
    REQUIRE_NOTHROW(ls.delete_object("a/b/hello.txt"));
    ls.delete_object("blob.bin");
}

//------------------------------------------------------------------------------

TEST_CASE("LocalStorage signs presigned URLs when a secret is set", "[storage][local]") {
    LocalConfig c = sample_local();
    c.secret = "local-signing-secret";
    LocalStorage ls(c);

    const std::string url = ls.presigned_url("obj.png", 60);

    // The signed URL is the object URL plus expiry + signature params.
    REQUIRE(url.rfind("/files/obj.png?expires=", 0) == 0);

    // Unsigned public URLs are disabled while signing is enforced...
    REQUIRE_THROWS_AS(ls.public_url("obj.png"), StorageError);
    // ...and put_object returns the key; presigned_url is the working link.
    REQUIRE(ls.put_object("obj.png", "x", "image/png") == "obj.png");
    ls.delete_object("obj.png");

    const std::string sig_marker = "&sig=";
    const std::size_t sig_pos = url.find(sig_marker);
    REQUIRE(sig_pos != std::string::npos);

    // expires is a positive unix timestamp in the future.
    const std::size_t exp_pos = url.find("expires=") + std::string("expires=").size();
    const long expires = std::strtol(url.substr(exp_pos, sig_pos - exp_pos).c_str(), nullptr, 10);
    REQUIRE(expires > 0);

    // sig is a 64-char lowercase-hex HMAC-SHA256.
    const std::string sig = url.substr(sig_pos + sig_marker.size());
    REQUIRE(sig.size() == 64);
    for (char ch : sig) REQUIRE(((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')));

    // Repeated calls within the cache window return the identical (stable) URL.
    REQUIRE(ls.presigned_url("obj.png", 60) == url);

    // A different lifetime is a distinct cache entry, so it re-signs.
    REQUIRE(ls.presigned_url("obj.png", 120) != url);

    // With no url_prefix this server doesn't serve the object, so there is no
    // mount to honour a signature: fall back to the (file://) public URL.
    c.url_prefix.clear();
    LocalStorage file_ls(c);
    REQUIRE(file_ls.presigned_url("obj.png", 60) == file_ls.public_url("obj.png"));
}

//------------------------------------------------------------------------------

TEST_CASE("put_user_object stores the object plus a .meta sidecar", "[storage][local]") {
    LocalConfig c;
    c.root = "_local/test/storage_user";
    LocalStorage ls(c);

    ObjectMeta om;
    om.filename = "notes.txt";
    const std::string key = ls.put_user_object(7, "hello", "text/plain", om);

    // Key shape: <27-char user seg>/<shard>/<27-char digest> + content suffix,
    // where the shard is the digest's first char (the 64-way per-user fan-out).
    REQUIRE(key.size() == 27 + 1 + 1 + 1 + 27 + 4);
    REQUIRE(key[27] == '/');
    REQUIRE(key[29] == '/');
    REQUIRE(key[28] == key[30]);   // shard == digest[0]
    REQUIRE(key.compare(key.size() - 4, 4, ".txt") == 0);
    REQUIRE(ls.get_object(key) == "hello");

    // The sidecar records the storage-level metadata plus the ObjectMeta --
    // and never the user id, which stored objects must not leak.
    const std::string meta = ls.get_object(key + ".meta");
    REQUIRE_FALSE(contains(meta, "user_id"));
    REQUIRE(contains(meta, "\"content_type\":\"text/plain\""));
    REQUIRE(contains(meta, "\"size\":5"));
    REQUIRE(contains(meta, "\"uploaded_at\":"));
    REQUIRE(contains(meta, "\"filename\":\"notes.txt\""));

    // Both ride under the user's prefix; identical bytes dedup to one object,
    // and the re-upload APPENDS a second meta record (JSON Lines) instead of
    // rewriting the sidecar.
    REQUIRE(ls.put_user_object(7, "hello", "text/plain", om) == key);
    REQUIRE(ls.list_user_objects(7).size() == 2);
    const std::string meta2 = ls.get_object(key + ".meta");
    REQUIRE(std::count(meta2.begin(), meta2.end(), '\n') == 2);

    // Empty ObjectMeta fields are omitted from the sidecar.
    const std::string bare = ls.put_user_object(7, "world", "text/plain");
    REQUIRE_FALSE(contains(ls.get_object(bare + ".meta"), "\"filename\""));
}

//------------------------------------------------------------------------------

TEST_CASE("append_to_object creates then extends an object", "[storage][local]") {
    LocalConfig c;
    c.root = "_local/test/storage_append";
    LocalStorage ls(c);

    // Appending to a missing key creates it.
    REQUIRE(ls.append_to_object("log.txt", "one\n", "text/plain") == "log.txt");
    REQUIRE(ls.get_object("log.txt") == "one\n");

    // Appending again extends, never replaces.
    ls.append_to_object("log.txt", "two\n", "text/plain");
    REQUIRE(ls.get_object("log.txt") == "one\ntwo\n");

    // Binary-safe: embedded NULs survive the read-modify-write.
    const std::string binary("\x00\xff", 2);
    ls.append_to_object("log.txt", binary, "application/octet-stream");
    REQUIRE(ls.get_object("log.txt") == "one\ntwo\n" + binary);

    // object_exists is an exact-key probe: a bare prefix of a key is not an
    // object.
    REQUIRE(ls.object_exists("log.txt"));
    REQUIRE_FALSE(ls.object_exists("log"));
    REQUIRE_FALSE(ls.object_exists("nope.txt"));

    // The base read-modify-write (what backends without a native append use)
    // behaves identically; call it explicitly since LocalStorage overrides.
    REQUIRE(ls.Storage::append_to_object("rmw.txt", "a", "text/plain") == "rmw.txt");
    ls.Storage::append_to_object("rmw.txt", "b", "text/plain");
    REQUIRE(ls.get_object("rmw.txt") == "ab");
}

//------------------------------------------------------------------------------

TEST_CASE("LocalStorage url_prefix is a URL path, not an on-disk prefix", "[storage][local]") {
    LocalStorage ls(sample_local());   // url_prefix "/files", root _local/test/storage
    ls.put_object("chart.png", "x", "image/png");

    // The URL carries the prefix...
    REQUIRE(ls.public_url("chart.png") == "/files/chart.png");
    // ...but on disk the object sits directly under root, NOT under root/files.
    REQUIRE(file_on_disk("_local/test/storage/chart.png"));
    REQUIRE_FALSE(file_on_disk("_local/test/storage/files/chart.png"));

    ls.delete_object("chart.png");
}

//------------------------------------------------------------------------------

TEST_CASE("LocalStorage applies the configured key prefix", "[storage][local]") {
    LocalConfig c;
    c.root       = "_local/test/storage_keyprefix";
    c.prefix     = "objects";
    c.url_prefix = "/files";
    LocalStorage ls(c);

    // put_object returns a PREFIX-LESS key: the configured prefix is an
    // internal access detail, not part of returned keys. Reads accept both the
    // prefix-less and the (older) prefixed form (full_key is idempotent).
    REQUIRE(ls.put_object("a/b.txt", "x", "text/plain") == "a/b.txt");
    REQUIRE(ls.get_object("a/b.txt") == "x");
    REQUIRE(ls.get_object("objects/a/b.txt") == "x");
    REQUIRE(ls.full_key("a/b.txt") == "objects/a/b.txt");
    REQUIRE(ls.full_key("objects/a/b.txt") == "objects/a/b.txt");

    // On disk the object sits under root/<prefix>/...
    REQUIRE(file_on_disk("_local/test/storage_keyprefix/objects/a/b.txt"));

    // ...and the URL carries it (the mount strips /files, then resolves
    // objects/a/b.txt under root) -- identically for both key forms.
    REQUIRE(ls.public_url("a/b.txt") == "/files/objects/a/b.txt");
    REQUIRE(ls.public_url("objects/a/b.txt") == "/files/objects/a/b.txt");

    // Listings accept either key form and return prefix-less keys.
    const std::vector<std::string> keys = ls.list_objects("a/", 10);
    REQUIRE(keys.size() == 1);
    REQUIRE(keys[0] == "a/b.txt");
    REQUIRE(ls.list_objects("objects/a/", 10) == keys);

    // object_exists probes either form too.
    REQUIRE(ls.object_exists("a/b.txt"));
    REQUIRE(ls.object_exists("objects/a/b.txt"));

    // A traversal smuggled via the configured prefix is rejected too.
    LocalConfig bad = c;
    bad.prefix = "../escape";
    LocalStorage bs(bad);
    REQUIRE_THROWS_AS(bs.put_object("x.txt", "x", "text/plain"), StorageError);
}

//------------------------------------------------------------------------------

TEST_CASE("LocalStorage advertises an absolute base_url for CDN delivery", "[storage][local]") {
    LocalConfig c = sample_local();            // url_prefix "/files"
    c.base_url = "https://cdn.example.com";    // host only; url_prefix is appended for you
    LocalStorage ls(c);

    // base_url supplies the scheme://host; url_prefix ("/files") is appended
    // automatically, then the bare key. The CDN forwards "/files/obj.png" to
    // origin, where the mount serves the same object. (A trailing slash on
    // base_url is tolerated and does not double the separator.)
    REQUIRE(ls.public_url("obj.png") == "https://cdn.example.com/files/obj.png");

    c.base_url = "https://cdn.example.com/";   // trailing slash trimmed, no "//"
    REQUIRE(LocalStorage(c).public_url("obj.png") == "https://cdn.example.com/files/obj.png");
}

//------------------------------------------------------------------------------

TEST_CASE("LocalStorage uses a file:// URL when neither URL prefix is set", "[storage][local]") {
    LocalConfig c = sample_local();
    c.url_prefix.clear();
    LocalStorage ls(c);

    const std::string url = ls.public_url("obj.png");
    REQUIRE(url.rfind("file://", 0) == 0);
    REQUIRE(url.find("obj.png") != std::string::npos);
}

//------------------------------------------------------------------------------

TEST_CASE("LocalStorage rejects keys that escape the root", "[storage][local]") {
    LocalStorage ls(sample_local());
    REQUIRE_THROWS_AS(ls.public_url("../escape.txt"), StorageError);
    REQUIRE_THROWS_AS(ls.get_object("a/../../etc/passwd"), StorageError);
    REQUIRE_THROWS_AS(ls.put_object("", "x", "text/plain"), StorageError);
}

//------------------------------------------------------------------------------

TEST_CASE("LocalStorage lists objects by prefix", "[storage][local]") {
    LocalConfig c;
    c.root = "_local/test/storage_list";
    LocalStorage ls(c);

    ls.put_object("u1/aaa/b.pdf",      "x", "application/pdf");
    ls.put_object("u1/aaa/b.pdf.json", "x", "application/json");
    ls.put_object("u1/bbb/c.png",      "x", "image/png");
    ls.put_object("u2/aaa/d.pdf",      "x", "application/pdf");

    // Prefix scan stays inside the prefix, results sorted lexicographically.
    // (The default max_keys lives on the Storage base, so the concrete type
    // takes it explicitly here.)
    const std::vector<std::string> u1 = ls.list_objects("u1/", 1000);
    REQUIRE(u1.size() == 3);
    REQUIRE(u1[0] == "u1/aaa/b.pdf");
    REQUIRE(u1[1] == "u1/aaa/b.pdf.json");
    REQUIRE(u1[2] == "u1/bbb/c.png");

    // A partial last segment also works (plain string prefix, not a dir).
    REQUIRE(ls.list_objects("u1/aaa/b.pdf.", 1000).size() == 1);

    // max_keys caps the result; an unknown prefix is empty, not an error.
    REQUIRE(ls.list_objects("u1/", 2).size() == 2);
    REQUIRE(ls.list_objects("nope/", 1000).empty());

    // A traversal prefix is rejected like any other key.
    REQUIRE_THROWS_AS(ls.list_objects("../escape/", 1000), StorageError);
}

//------------------------------------------------------------------------------

TEST_CASE("file encryption stores ciphertext at rest, decrypts on read", "[storage][encrypt]") {
    LocalConfig c;
    c.root = "_local/test/storage_encrypt";
    LocalStorage ls(c);

    auto cipher = std::make_shared<mirobody::encrypt::Fernet>(
        mirobody::encrypt::Fernet::generate_key());
    ls.enable_file_encryption({cipher}, "object-key-seed-secret", "/files", "", "url-signing-secret");
    REQUIRE(ls.file_encryption_enabled());

    // Object-key derivation switches to the secret seed (not the bucket name),
    // so a credential-holder who knows the bucket can't recompute the keys.
    REQUIRE(ls.object_key_seed() == "object-key-seed-secret");

    const std::string plaintext = "secret report contents";
    ObjectMeta om;
    om.filename = "report.pdf";
    const std::string key = ls.put_user_object(7, plaintext, "application/pdf", om);

    // The object KEY is still derived from the PLAINTEXT (content-addressed
    // dedup survives encryption): a re-upload of identical bytes lands on the
    // same key.
    REQUIRE(ls.put_user_object(7, plaintext, "application/pdf", om) == key);

    // On disk the bytes are a Fernet token, never the plaintext...
    const std::string raw = ls.get_object(key);
    REQUIRE(raw != plaintext);
    REQUIRE_FALSE(contains(raw, "report contents"));
    REQUIRE(raw.rfind("gAAAAA", 0) == 0);   // Fernet token prefix (version 0x80)
    // ...but the read seam recovers it.
    REQUIRE(ls.get_object_decrypted(key) == plaintext);

    // The .meta sidecar is encrypted too (the filename must not leak), yet a
    // re-upload still appends a second JSON-Lines record under the encryption.
    const std::string raw_meta = ls.get_object(key + ".meta");
    REQUIRE_FALSE(contains(raw_meta, "report.pdf"));
    REQUIRE_FALSE(contains(raw_meta, "content_type"));
    const std::string meta = ls.get_object_decrypted(key + ".meta");
    REQUIRE(contains(meta, "\"filename\":\"report.pdf\""));
    REQUIRE(std::count(meta.begin(), meta.end(), '\n') == 2);

    // put_object_encrypted / get_object_decrypted round-trip a .trans sidecar.
    ls.put_object_encrypted(key + ".trans", "extracted text", "text/plain; charset=utf-8");
    REQUIRE(ls.get_object(key + ".trans") != "extracted text");
    REQUIRE(ls.get_object_decrypted(key + ".trans") == "extracted text");

    // A signed read URL points at the app file mount (not the local disk URL),
    // carrying the full key plus an expiring HMAC the router mount verifies.
    const std::string url = ls.signed_read_url(key);
    REQUIRE(url.rfind("/files/", 0) == 0);
    REQUIRE(contains(url, key));
    REQUIRE(contains(url, "expires="));
    REQUIRE(contains(url, "sig="));
}

//------------------------------------------------------------------------------

TEST_CASE("multi-key: newest encrypts, any configured key decrypts", "[storage][encrypt]") {
    LocalConfig c;
    c.root = "_local/test/storage_multikey";

    auto k1 = std::make_shared<mirobody::encrypt::Fernet>(mirobody::encrypt::Fernet::generate_key());
    auto k2 = std::make_shared<mirobody::encrypt::Fernet>(mirobody::encrypt::Fernet::generate_key());

    // Write an object under k1 alone.
    std::string key;
    {
        LocalStorage ls(c);
        ls.enable_file_encryption({k1}, "seed", "/files", "", "url-secret");
        key = ls.put_user_object(7, "old-key payload", "text/plain");
    }
    // Rotate: append k2 (now the encrypt key), keep k1 for decrypt. The old
    // object -- written under k1 -- still reads, because all keys are tried.
    {
        LocalStorage ls(c);
        ls.enable_file_encryption({k1, k2}, "seed", "/files", "", "url-secret");
        REQUIRE(ls.get_object_decrypted(key) == "old-key payload");
        // New writes use k2 (the last key); still transparently readable.
        const std::string key2 = ls.put_user_object(7, "new-key payload", "text/plain");
        REQUIRE(ls.get_object_decrypted(key2) == "new-key payload");
    }
    // Drop k1, keep only k2: the k1-encrypted object can no longer be read
    // (this is why an old key must not be dropped until its objects are gone).
    {
        LocalStorage ls(c);
        ls.enable_file_encryption({k2}, "seed", "/files", "", "url-secret");
        REQUIRE_THROWS_AS(ls.get_object_decrypted(key), StorageError);
    }
}

//------------------------------------------------------------------------------

TEST_CASE("reconcile_key_marker guards the seed and key set", "[storage][encrypt]") {
    using mirobody::storage::reconcile_key_marker;
    using mirobody::storage::KeyMarkerCheck;

    // First run (no marker): ok, and it records the seed + keys.
    KeyMarkerCheck first = reconcile_key_marker("", "seedFP", {"keyA"});
    REQUIRE(first.ok);
    REQUIRE_FALSE(first.updated_marker.empty());
    const std::string marker = first.updated_marker;

    // Same config again: ok, nothing new to record.
    KeyMarkerCheck same = reconcile_key_marker(marker, "seedFP", {"keyA"});
    REQUIRE(same.ok);
    REQUIRE(same.updated_marker.empty());

    // Rotation -- append keyB (keyA still present): ok, and the set grows so a
    // later drop of keyA is still recognized as a known rotation.
    KeyMarkerCheck rotated = reconcile_key_marker(marker, "seedFP", {"keyA", "keyB"});
    REQUIRE(rotated.ok);
    REQUIRE_FALSE(rotated.updated_marker.empty());
    // After the merge, dropping keyA (only keyB configured) is accepted.
    KeyMarkerCheck dropped = reconcile_key_marker(rotated.updated_marker, "seedFP", {"keyB"});
    REQUIRE(dropped.ok);

    // Changed seed -> refuse (would re-path every object).
    KeyMarkerCheck seed_bad = reconcile_key_marker(marker, "DIFFERENT", {"keyA"});
    REQUIRE_FALSE(seed_bad.ok);
    REQUIRE_FALSE(seed_bad.reason.empty());

    // Whole key list replaced with unrelated keys -> refuse.
    KeyMarkerCheck keys_bad = reconcile_key_marker(marker, "seedFP", {"keyZ"});
    REQUIRE_FALSE(keys_bad.ok);
}

//------------------------------------------------------------------------------

TEST_CASE("key marker round-trips through storage", "[storage][encrypt]") {
    LocalConfig c;
    c.root = "_local/test/storage_marker";
    std::remove("_local/test/storage_marker/.file-encryption-check");

    LocalStorage ls(c);
    REQUIRE(ls.read_key_marker().empty());          // absent => ""
    ls.write_key_marker("seed=x\nkey=y\n");
    REQUIRE(ls.read_key_marker() == "seed=x\nkey=y\n");
}

//------------------------------------------------------------------------------

TEST_CASE("file encryption disabled is a passthrough", "[storage][encrypt]") {
    LocalConfig c;
    c.root = "_local/test/storage_noencrypt";
    LocalStorage ls(c);   // no enable_file_encryption call

    REQUIRE_FALSE(ls.file_encryption_enabled());
    const std::string key = ls.put_user_object(7, "plain", "text/plain");
    // Stored verbatim; the *_encrypted / *_decrypted seams are no-ops.
    REQUIRE(ls.get_object(key) == "plain");
    REQUIRE(ls.get_object_decrypted(key) == "plain");
    ls.put_object_encrypted("k.trans", "txt", "text/plain");
    REQUIRE(ls.get_object("k.trans") == "txt");
    // signed_read_url collapses to presigned_url (here the local mount URL).
    REQUIRE(ls.signed_read_url(key, 3600) == ls.presigned_url(key, 3600));
}
