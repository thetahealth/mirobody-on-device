#include "transcode/file.hpp"

#include "cache/cache.hpp"
#include "database/database.hpp"
#include "storage/sign.hpp"
#include "storage/storage.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using mirobody::cache::Cache;
using mirobody::file::FileRef;
namespace file = mirobody::file;

namespace {

FileRef make_ref(const std::string& key, const std::string& name,
                 const std::string& mime = "application/pdf") {
    FileRef f;
    f.filename  = name;
    f.mime_type = mime;
    f.file_key  = key;
    f.url       = "https://cdn.example/" + key;
    return f;
}

// A LocalStorage backend on the shared test scratch tree (removed at run end
// by the listener in tests/storage/storage_test.cpp).
std::unique_ptr<mirobody::storage::Storage> make_storage() {
    mirobody::storage::LocalConfig c;
    c.root = "_local/test/file_index";
    return c.open();
}

// The per-user prefix Storage::put_user_object derives: unpadded-base64url
// HMAC-SHA1 of the user id, keyed by "" -- LocalStorage's bucket_name(),
// matching make_storage.
std::string user_seg(std::int64_t user_id) {
    const std::array<unsigned char, 20> mac =
        mirobody::storage::hmac_sha1("", std::to_string(user_id));
    return mirobody::storage::base64url_encode(
        std::string(reinterpret_cast<const char*>(mac.data()), mac.size()));
}

// A server-shaped key for `user_id`:
// <user_seg>/<shard>/<27-char base64url digest>[.<ext>], the form the upload
// path produces (and the only one the sidecar fallbacks accept). The shard is
// the digest's first char (here `digest_char`); `ext` is the content-type-
// derived suffix, including its dot ("" for none).
std::string shaped_key(std::int64_t user_id, char digest_char, const std::string& ext = "") {
    return user_seg(user_id) + "/" + std::string(1, digest_char) + "/" +
           std::string(27, digest_char) + ext;
}

// One record of the <key>.meta sidecar (JSON Lines), as
// Storage::put_user_object appends it -- call repeatedly to simulate
// re-uploads of the same bytes.
void put_meta(mirobody::storage::Storage& s, const std::string& key,
              const std::string& filename, std::int64_t uploaded_at) {
    s.append_to_object(key + ".meta",
                       "{\"content_type\":\"application/pdf\",\"size\":3,"
                       "\"uploaded_at\":" + std::to_string(uploaded_at) + ","
                       "\"filename\":\"" + filename + "\"}\n",
                       "application/x-ndjson");
}

}   // namespace

TEST_CASE("record then list round-trips a user's uploads", "[file]") {
    Cache cache;   // in-process backend
    REQUIRE(file::list(cache, nullptr, 42).empty());

    file::record(cache, 42, make_ref("h/a/report.pdf", "report.pdf"));
    file::record(cache, 42, make_ref("h/b/scan.png", "scan.png", "image/png"));

    const std::vector<FileRef> files = file::list(cache, nullptr, 42);
    REQUIRE(files.size() == 2);
    REQUIRE(files[0].file_key == "h/b/scan.png");   // newest first
    REQUIRE(files[0].mime_type == "image/png");
    REQUIRE(files[1].filename == "report.pdf");
}

TEST_CASE("the JSON rendering is re-rendered from the list", "[file]") {
    Cache cache;
    file::record(cache, 33, make_ref("h/a/x.pdf", "x.pdf"));
    file::record(cache, 33, make_ref("h/b/y.pdf", "y.pdf"));

    // Drop only the rendered JSON, as its (short) TTL expiring would; the
    // list rows survive and the next read re-renders from them. The key
    // names are the documented cache contract in transcode/file.cpp.
    REQUIRE(cache.del("mirobody:user:files:json:33"));
    REQUIRE(cache.exists("mirobody:user:files:33"));

    const std::vector<FileRef> files = file::list(cache, nullptr, 33);
    REQUIRE(files.size() == 2);
    REQUIRE(files[0].file_key == "h/b/y.pdf");
    // ...and the rendering was re-cached for the next read.
    REQUIRE(cache.exists("mirobody:user:files:json:33"));
}

TEST_CASE("record dedups by file_key", "[file]") {
    Cache cache;
    file::record(cache, 7, make_ref("h/a/x.pdf", "x.pdf"));
    file::record(cache, 7, make_ref("h/a/x.pdf", "x.pdf"));   // same key
    REQUIRE(file::list(cache, nullptr, 7).size() == 1);
}

TEST_CASE("record stamps uploaded_at once; dedup keeps the first stamp", "[file]") {
    Cache cache;
    file::record(cache, 21, make_ref("h/a/x.pdf", "x.pdf"));

    FileRef out;
    REQUIRE(file::find(cache, nullptr, 21, "h/a/x.pdf", out));
    REQUIRE(out.uploaded_at > 0);   // stamped at record time

    FileRef f = make_ref("h/b/y.pdf", "y.pdf");
    f.uploaded_at = 1000;                      // explicit caller value wins
    file::record(cache, 21, f);
    file::record(cache, 21, make_ref("h/b/y.pdf", "y.pdf"));   // re-upload
    REQUIRE(file::find(cache, nullptr, 21, "h/b/y.pdf", out));
    REQUIRE(out.uploaded_at == 1000);          // not refreshed by the dup
}

TEST_CASE("extracted-text key round-trips through the cache", "[file]") {
    Cache cache;
    FileRef f = make_ref("h/c/report.pdf", "report.pdf");
    f.text_key = "h/c/report.pdf.trans";   // as the upload path would have set it
    file::record(cache, 5, f);

    FileRef out;
    REQUIRE(file::find(cache, nullptr, 5, "h/c/report.pdf", out));
    REQUIRE(out.text_key == "h/c/report.pdf.trans");
}

TEST_CASE("record drops the oldest past kMaxFiles", "[file]") {
    Cache cache;
    for (std::size_t i = 0; i <= file::kMaxFiles; ++i) {   // one past the cap
        FileRef f = make_ref("h/k/" + std::to_string(i), std::to_string(i));
        f.uploaded_at = static_cast<std::int64_t>(i + 1);
        file::record(cache, 99, f);
    }
    const std::vector<FileRef> files = file::list(cache, nullptr, 99);
    REQUIRE(files.size() == file::kMaxFiles);
    REQUIRE(files.front().file_key ==
            "h/k/" + std::to_string(file::kMaxFiles));   // newest first
    REQUIRE(files.back().file_key == "h/k/1");           // "h/k/0" trimmed away
}

TEST_CASE("record is a no-op for anonymous / empty key", "[file]") {
    Cache cache;
    file::record(cache, 0, make_ref("h/a/x.pdf", "x.pdf"));   // user_id <= 0
    file::record(cache, 9, make_ref("", "x.pdf"));            // empty key
    REQUIRE(file::list(cache, nullptr, 0).empty());
    REQUIRE(file::list(cache, nullptr, 9).empty());
}

TEST_CASE("an upload extends a sidecar-rebuilt list", "[file]") {
    std::unique_ptr<mirobody::storage::Storage> storage = make_storage();
    const std::string first = shaped_key(71, 'a', ".pdf");
    put_meta(*storage, first, "first.pdf", 1000);

    Cache cache;
    REQUIRE(file::list(cache, storage.get(), 71).size() == 1);   // cold rebuild
    file::record(cache, 71, make_ref(shaped_key(71, 'b', ".pdf"), "second.pdf"));

    const std::vector<FileRef> files = file::list(cache, nullptr, 71);
    REQUIRE(files.size() == 2);
    REQUIRE(files[0].filename == "second.pdf");   // newest first
    REQUIRE(files[1].filename == "first.pdf");
}

TEST_CASE("iso8601_utc formats unix seconds", "[file]") {
    REQUIRE(file::iso8601_utc(0) == "1970-01-01T00:00:00Z");
    REQUIRE(file::iso8601_utc(86400 + 3661) == "1970-01-02T01:01:01Z");
}

TEST_CASE("list drops the oldest past kMaxFiles", "[file]") {
    std::unique_ptr<mirobody::storage::Storage> storage = make_storage();
    for (std::size_t i = 0; i <= file::kMaxFiles; ++i) {   // one past the cap
        // Distinct server-shaped keys: a 27-char digest with `i` encoded in
        // the tail (digits are valid base64url characters). All share the 'a'
        // shard (digest first char), which is fine -- this exercises the cap,
        // not the shard fan-out.
        const std::string n = std::to_string(i);
        const std::string digest = std::string(27 - n.size(), 'a') + n;
        const std::string key = user_seg(81) + "/" + digest.substr(0, 1) + "/" + digest + ".pdf";
        put_meta(*storage, key, n, static_cast<std::int64_t>(i + 1));
    }
    Cache cold;
    const std::vector<FileRef> files = file::list(cold, storage.get(), 81);
    REQUIRE(files.size() == file::kMaxFiles);
    REQUIRE(files.front().uploaded_at ==
            static_cast<std::int64_t>(file::kMaxFiles + 1));   // newest first
    REQUIRE(files.back().uploaded_at == 2);                    // stamp 1 trimmed
}

TEST_CASE("find enforces per-user ownership", "[file]") {
    std::unique_ptr<mirobody::storage::Storage> storage = make_storage();
    const std::string key = shaped_key(1, 'a', ".pdf");
    put_meta(*storage, key, "mine.pdf", 1);

    Cache cache;
    REQUIRE(file::list(cache, storage.get(), 1).size() == 1);   // warm the cache

    FileRef out;
    REQUIRE(file::find(cache, nullptr, 1, key, out));   // resolved from the cache
    REQUIRE(out.filename == "mine.pdf");

    // A different user can't resolve user 1's key: it is not in their (empty)
    // list and the sidecar fallback rejects the foreign key shape.
    REQUIRE_FALSE(file::find(cache, storage.get(), 2, key, out));
    // An unknown key for the right user also misses.
    REQUIRE_FALSE(file::find(cache, nullptr, 1, shaped_key(1, 'z', ".pdf"), out));
}

TEST_CASE("find falls back to the per-file sidecar", "[file]") {
    std::unique_ptr<mirobody::storage::Storage> storage = make_storage();
    const std::string key = shaped_key(11, 'a', ".pdf");

    // What put_user_object would have stored next to the upload.
    put_meta(*storage, key, "report.pdf", 1234);

    // A fresh cache simulates the expired list: the key still resolves with
    // full metadata through the <file_key>.meta sidecar; url and
    // text_key are derived server-side, not read from it.
    Cache cold;
    FileRef out;
    REQUIRE(file::find(cold, storage.get(), 11, key, out));
    REQUIRE(out.filename == "report.pdf");
    REQUIRE(out.mime_type == "application/pdf");
    REQUIRE(out.uploaded_at == 1234);   // the stamp survives in the sidecar
    REQUIRE(out.text_key == key + ".trans");   // optimistic; reads fall back
    REQUIRE_FALSE(out.url.empty());

    // A re-upload appends a record: the latest filename wins, the original
    // upload time survives in the first record.
    put_meta(*storage, key, "renamed.pdf", 9999);
    Cache cold2;
    REQUIRE(file::find(cold2, storage.get(), 11, key, out));
    REQUIRE(out.filename == "renamed.pdf");
    REQUIRE(out.uploaded_at == 1234);

    // Another user can't resolve it: the key fails their shape check.
    REQUIRE_FALSE(file::find(cold, storage.get(), 12, key, out));
}

TEST_CASE("list rebuilds the cache from sidecars", "[file]") {
    std::unique_ptr<mirobody::storage::Storage> storage = make_storage();

    const std::string newer = shaped_key(51, 'a', ".pdf");
    const std::string older = shaped_key(51, 'b', ".pdf");
    put_meta(*storage, newer, "newer.pdf", 2000);
    put_meta(*storage, older, "older.pdf", 1000);
    // Only `older` had text extracted.
    storage->put_object(older + ".trans", "text", "text/plain");

    // Cold cache: the listing comes back from the sidecar scan, newest first,
    // with text_key set only where the extracted text actually exists.
    Cache cold;
    const std::vector<FileRef> files = file::list(cold, storage.get(), 51);
    REQUIRE(files.size() == 2);
    REQUIRE(files[0].filename == "newer.pdf");
    REQUIRE(files[1].filename == "older.pdf");
    REQUIRE(files[1].uploaded_at == 1000);
    REQUIRE(files[1].text_key == older + ".trans");
    REQUIRE(files[0].text_key.empty());
    REQUIRE_FALSE(files[1].url.empty());   // minted at rebuild, not stored

    // The rebuild re-warmed the cache: a cache-only read now succeeds.
    REQUIRE(file::list(cold, nullptr, 51).size() == 2);
}

TEST_CASE("list rebuild ignores objects that are not sidecars", "[file]") {
    std::unique_ptr<mirobody::storage::Storage> storage = make_storage();

    // A real upload's sidecar...
    const std::string real = shaped_key(61, 'a', ".pdf");
    put_meta(*storage, real, "real.pdf", 1);

    // ...plus a user-uploaded *.json file (an application/json upload stores
    // as <digest>.json, so its bytes are attacker-authored) and a malformed
    // .meta: neither becomes an index entry.
    storage->put_object(shaped_key(61, 'b', ".json"),
                        "{\"filename\":\"evil\",\"uploaded_at\":1}",
                        "application/json");
    storage->put_object(shaped_key(61, 'c', ".xml") + ".meta",
                        "not json", "application/json");

    Cache cold;
    const std::vector<FileRef> files = file::list(cold, storage.get(), 61);
    REQUIRE(files.size() == 1);
    REQUIRE(files[0].filename == "real.pdf");
}

TEST_CASE("crafted sidecar content cannot redirect reads", "[file]") {
    std::unique_ptr<mirobody::storage::Storage> storage = make_storage();
    const std::string key    = shaped_key(31, 'b', ".pdf");
    const std::string victim = shaped_key(30, 'c', ".pdf");

    // A sidecar whose (client-influenced) content smuggles foreign keys and a
    // foreign URL: file_key / text_key / url are derived server-side from the
    // requested key, never read from the document.
    storage->put_object(key + ".meta",
        "{\"content_type\":\"application/pdf\",\"filename\":\"a.pdf\","
        "\"file_key\":\"" + victim + "\","
        "\"text_key\":\"" + victim + ".trans\","
        "\"url\":\"https://evil.example/x\",\"uploaded_at\":1}",
        "application/json");

    Cache cache;
    FileRef out;
    REQUIRE(file::find(cache, storage.get(), 31, key, out));
    REQUIRE(out.file_key == key);
    REQUIRE(out.text_key == key + ".trans");
    REQUIRE(out.url.find("evil.example") == std::string::npos);
}

TEST_CASE("sidecar fallback rejects malformed keys", "[file]") {
    std::unique_ptr<mirobody::storage::Storage> storage = make_storage();
    Cache cache;
    FileRef out;
    const std::string seg = user_seg(41);
    const std::string digest(27, 'c');   // first char 'c' => the shard must be 'c'

    // Traversal, missing shard, mismatched shard, extra depth, short digest, bad
    // suffix shapes, foreign segment -- all rejected before a storage round-trip.
    REQUIRE_FALSE(file::find(cache, storage.get(), 41, seg + "/../escape", out));
    REQUIRE_FALSE(file::find(cache, storage.get(), 41, seg + "/" + digest + ".pdf", out));       // no shard
    REQUIRE_FALSE(file::find(cache, storage.get(), 41, seg + "/x/" + digest + ".pdf", out));     // shard != digest[0]
    REQUIRE_FALSE(file::find(cache, storage.get(), 41, seg + "/c/c/" + digest + ".pdf", out));   // extra depth
    REQUIRE_FALSE(file::find(cache, storage.get(), 41, seg + "/c/cc.pdf", out));                 // short digest
    REQUIRE_FALSE(file::find(cache, storage.get(), 41, seg + "/c/" + digest + ".", out));
    REQUIRE_FALSE(file::find(cache, storage.get(), 41, seg + "/c/" + digest + ".PDF", out));
    REQUIRE_FALSE(file::find(cache, storage.get(), 41, seg + "/c/" + digest + ".verylongext", out));
    REQUIRE_FALSE(file::find(cache, storage.get(), 41, "0123456789abcdef/c/" + digest + ".pdf", out));
}

//------------------------------------------------------------------------------

#if defined(MIROBODY_DATABASE_SQLITE)
// Exercises the real files-table DAO SQL (the cross-dialect upsert with a
// partial-index ON CONFLICT target, list sort/paginate, soft delete) against a
// live SQLite database. SQLite-guarded: SQLiteConfig::open() links only in the
// SQLite backend build.
TEST_CASE("files-table DAO upserts, lists, sorts, paginates", "[file][db]") {
    namespace db = mirobody::database;
    // Constructing a LocalStorage make_dirs() the scratch tree, so the SQLite
    // file's parent directory exists (sqlite won't create parents).
    std::unique_ptr<mirobody::storage::Storage> scratch = make_storage();
    (void)scratch;
    std::remove("_local/test/file_index/files_dao.db");
    db::SQLiteConfig sc;
    sc.path = "_local/test/file_index/files_dao.db";
    db::Database conn = sc.open();

    // The upsert needs the partial unique index to detect conflicts.
    conn.execute(
        "CREATE TABLE files ("
        " id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL, file_key TEXT NOT NULL,"
        " filename TEXT, content_type TEXT, text_key TEXT, summary TEXT,"
        " size_bytes INTEGER NOT NULL DEFAULT 0,"
        " created_at INTEGER NOT NULL,"
        " updated_at INTEGER,"
        " deleted_at INTEGER);");
    conn.execute(
        "CREATE UNIQUE INDEX uq_files_file_key_active ON files (file_key) WHERE deleted_at IS NULL;");

    file::db_upsert_file(conn, 7, "k_b", "b.pdf", "application/pdf", 20);
    file::db_upsert_file(conn, 7, "k_a", "a.pdf", "application/pdf", 10);
    REQUIRE(file::db_count_files(conn, 7) == 2);

    // Re-upload of k_a (same file_key) upserts: still 2 rows, fields updated.
    file::db_upsert_file(conn, 7, "k_a", "a-renamed.pdf", "application/pdf", 11);
    REQUIRE(file::db_count_files(conn, 7) == 2);

    // text_key is filled by the later step.
    file::db_set_file_text_key(conn, 7, "k_a", "k_a.trans");

    // Sort by filename ascending: "a-renamed.pdf" < "b.pdf".
    std::vector<file::FileRow> byname = file::db_list_files(conn, 7, "filename", false, 10, 0);
    REQUIRE(byname.size() == 2);
    REQUIRE(byname[0].filename   == "a-renamed.pdf");
    REQUIRE(byname[0].file_key   == "k_a");
    REQUIRE(byname[0].text_key   == "k_a.trans");
    REQUIRE(byname[0].size_bytes == 11);
    REQUIRE(byname[1].filename   == "b.pdf");

    // Pagination: size 1 returns one row; offset 1 the next.
    REQUIRE(file::db_list_files(conn, 7, "filename", false, 1, 0).size() == 1);
    std::vector<file::FileRow> page2 = file::db_list_files(conn, 7, "filename", false, 1, 1);
    REQUIRE(page2.size() == 1);
    REQUIRE(page2[0].filename == "b.pdf");

    // Scoped per user.
    REQUIRE(file::db_count_files(conn, 9) == 0);

    // Soft delete drops it from the active list; a re-upload then starts a fresh
    // active row (the deleted one stays as history).
    conn.execute("UPDATE files SET deleted_at = 1 WHERE file_key = ?;",
                 {std::string("k_b")});
    REQUIRE(file::db_count_files(conn, 7) == 1);
    file::db_upsert_file(conn, 7, "k_b", "b.pdf", "application/pdf", 20);
    REQUIRE(file::db_count_files(conn, 7) == 2);
}
#endif
