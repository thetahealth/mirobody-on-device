#pragma once

// Per-user uploads index: the <file_key>.meta sidecars that
// Storage::put_user_object writes are the durable source of truth; the cache
// (Redis in production) holds two derived keys for cheap listing.
//
// The chat upload path (chat::Dispatcher::store_attachments) records every
// file it offloads to object storage here; the MCP service reads the list
// back to answer resources/list and validates ownership for resources/read.
// Keeping the key scheme and JSON shape in one module means the writer and
// the reader can't drift apart.
//
// Storage model:
//   - Sidecars: every stored upload has a <file_key>.meta (written by
//     Storage::put_user_object; filename, content type, size, upload time) --
//     the durable copy everything is rebuilt from. Ownership is the hashed
//     key prefix itself: the rebuild only lists the caller's segment, and
//     find() pins client-supplied keys to it with a structural shape check
//     before any fetch.
//   - Cache, two keys per user (newest upload first, see file.cpp for the
//     names):
//       1. a LIST, one FileRef JSON object per row. record() LPUSHes one
//          row per new upload -- an atomic append, so concurrent uploads
//          can't lose each other's entries the way rewriting a shared
//          value could. 7-day TTL, refreshed per upload, trimmed to
//          kMaxFiles.
//       2. a STRING, the JSON array rendering of those rows -- what list()
//          serves. record() re-renders it after each push as ONE atomic
//          server-side step (cache.set_join: a Lua script on Redis), so the
//          rendering always reflects a current list state; a short TTL
//          backstops writers that die mid-record.
//     When both are cold, list() rebuilds from the sidecars and writes them
//     back, guarded by a generation counter so a scan that an upload raced
//     never clobbers the fresher cache state.

#include "cache/cache.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mirobody {

namespace storage { class Storage; }
namespace database { class Database; }

namespace file {

// One uploaded file's metadata. `file_key` is the object-storage key (the read
// path fetches bytes by it); `url` is a signed GET URL minted at upload /
// lookup time (Storage::signed_read_url -- bucket-direct/local mount normally,
// an app-served decrypting endpoint when file encryption is on), kept for
// clients that prefer a link over inlined bytes (it may expire; re-mint from
// `file_key` for a fresh one). `text_key` is
// the object-storage key of the text extracted from the original at upload
// (see transcode/parser.hpp), stored alongside it as `<file_key>.trans` -- empty
// when extraction is disabled, failed, or the file is already plain text
// (find()'s sidecar fallback sets it optimistically; readers fall back to the
// bytes when the fetch 404s). `uploaded_at` is the unix time (seconds) the
// file was first uploaded: record() stamps it (dedup keeps the first row's
// stamp), and a rebuild from the .meta sidecar takes the FIRST record's
// stamp (a re-upload appends a record rather than rewriting).
struct FileRef {
    std::string  filename;
    std::string  mime_type;
    std::string  file_key;
    std::string  url;
    std::string  text_key;
    std::int64_t uploaded_at = 0;
};

// How long the cache list survives without a new upload (the JSON rendering
// has its own, much shorter TTL -- see file.cpp).
const std::chrono::hours kTtl(24 * 7);

// Cap on the number of files retained per user; the oldest are dropped past it
// so a prolific uploader can't grow the cache entry without bound. Trimmed
// files keep their objects and sidecars -- they just stop being listed.
const std::size_t kMaxFiles = 200;

// `secs` as an ISO 8601 UTC timestamp ("2026-06-10T18:00:00Z") -- how
// uploaded_at is surfaced to tools and MCP resource descriptors.
std::string iso8601_utc(std::int64_t secs);

// Record a new upload: LPUSH one row onto the caller's cache list (deduped
// by file_key, trimmed to kMaxFiles, TTL refreshed) and re-render the JSON
// from it, stamping uploaded_at on first sight. The durable copy is the
// .meta sidecar the upload already wrote. No-op when user_id <= 0 or
// f.file_key is empty.
void record(cache::Cache& cache, std::int64_t user_id, const FileRef& f);

// The caller's uploads, newest first: the rendered JSON when fresh, else
// re-rendered from the cache list, else (when `storage` is non-null) rebuilt
// from the <file_key>.meta sidecars under the caller's prefix and written
// back. Empty when nothing is stored or user_id <= 0.
std::vector<FileRef> list(cache::Cache& cache, storage::Storage* storage, std::int64_t user_id);

// Find one file by its object-storage key. Checks the caller's cached list
// first; on a miss, falls back to the <file_key>.meta sidecar (when
// `storage` is non-null), accepting the key only if it has this caller's
// exact shape ([<storage prefix>/]<user_segment>/<shard>/<27-char base64url
// digest>[.suffix], shard == digest's first char; the prefix-less form covers
// keys recorded before a prefix was configured). The hashed segment is the
// ownership check the read path
// relies on before fetching bytes, so one user can't read another's objects
// by guessing keys.
bool find(cache::Cache& cache, storage::Storage* storage, std::int64_t user_id,
          const std::string& file_key, FileRef& out);

//------------------------------------------------------------------------------
// Database-backed index (the `files` table)
//------------------------------------------------------------------------------
//
// A queryable secondary index over the same uploads, so a UI can sort (by
// upload time or filename), search, and paginate across a user's whole history
// -- which the cache/sidecar index above (newest-first, capped) can't. The
// .meta sidecars remain the durable source of truth; this table is rebuildable
// from them. Written by the upload paths (chat::Dispatcher and the C API's
// store), read by GET /api/files and the C API's list. The SQL is one dialect
// for every new-schema backend (pg, sqlite, ...); only PG_LEGACY keeps the old
// th_files table instead.

// One row of the `files` table, as db_list_files returns it.
struct FileRow {
    std::int64_t id = 0;
    std::string  filename;
    std::string  mime_type;    // `content_type` column
    std::string  file_key;
    std::string  text_key;     // "" when no extracted text yet
    std::string  summary;      // "" until post-upload processing fills it
    std::int64_t size_bytes = 0;
    std::int64_t created_at = 0;   // upload time, unix milliseconds
};

// Upsert the row for an upload. file_key is unique among active rows
// (content-addressed), so a re-upload updates the existing active row (keeping
// created_at) rather than duplicating it. No-op when user_id <= 0. Throws on a
// database error (callers treat indexing as best-effort and catch).
void db_upsert_file(database::Database& db, std::int64_t user_id,
                    const std::string& file_key, const std::string& filename,
                    const std::string& content_type, std::int64_t size_bytes);

// Fill text_key (the .trans object key) on the row after extraction finishes,
// bumping updated_at. No-op when user_id <= 0 or either key is empty.
void db_set_file_text_key(database::Database& db, std::int64_t user_id,
                          const std::string& file_key, const std::string& text_key);

// The caller's active files, sorted and paginated, for GET /api/files. `sort`
// is "filename" or anything else (=> created_at, the default); `descending`
// flips the order; `limit` / `offset` page. db_count_files is the matching
// total. Empty when user_id <= 0.
std::vector<FileRow> db_list_files(database::Database& db, std::int64_t user_id,
                                   const std::string& sort, bool descending,
                                   int limit, int offset);
std::int64_t db_count_files(database::Database& db, std::int64_t user_id);

}}
