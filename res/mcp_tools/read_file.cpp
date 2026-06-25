// MCP tool: read_file. Authentication required.
//
// Fetches one of the authenticated user's uploaded files by its file_key (as
// returned by list_files). Ownership is enforced through the per-user index:
// the key must be in the caller's own list, so one user can't read another's
// objects by guessing keys. Text-ish files are returned inline as `content`;
// other types (images, PDFs, binaries) return a `url` instead of inlining bytes
// the model can't use, keeping the tool result small.
//
// Reaches the cache (the index + ownership check) and the object store (the
// bytes / signed URL) through the ToolContext.

#include "mcp/tool.hpp"
#include "platform/log.hpp"
#include "storage/storage.hpp"
#include "transcode/file.hpp"

#include <rapidjson/document.h>

#include <string>

namespace {

using namespace mirobody::mcp;

// Whether a payload of `mime` is safe to inline as UTF-8 text.
bool is_text_mime(const std::string& mime) {
    if (mime.rfind("text/", 0) == 0) return true;
    return mime == "application/json" || mime == "application/xml" ||
           mime == "application/javascript" || mime == "image/svg+xml";
}

rapidjson::Value str(const std::string& s, rapidjson::Document::AllocatorType& a) {
    return rapidjson::Value(s.c_str(), static_cast<rapidjson::SizeType>(s.size()), a);
}

//------------------------------------------------------------------------------

Result read_file(const Args& args, const UserInfo& user, const ToolContext& ctx) {
    if (!user.authed()) {
        return Result::error("Not authenticated");
    }
    std::string file_key = args.str("file_key");
    mirobody::platform::log_debug("mcp[read_file]: user %lld key=%s", (long long)user.user_id, file_key.c_str());
    if (file_key.empty()) {
        return Result::error("read_file requires a 'file_key' (see list_files).");
    }
    if (mirobody::storage::is_meta_key(file_key) || mirobody::storage::is_trans_key(file_key)) {
        file_key = mirobody::storage::strip_sidecar_suffix(file_key);
        mirobody::platform::log_debug("mcp[read_file]: key has sidecar suffix; stripped to %s", file_key.c_str());
    }

    if (ctx.storage == nullptr) {
        return Result::error("Object storage is not configured.");
    }

    if (ctx.cache == nullptr) {
        return Result::error("File index is not available.");
    }

    mirobody::file::FileRef ref = {};
    if (!mirobody::file::find(*ctx.cache, ctx.storage, user.user_id, file_key, ref)) {
        return Result::error("File not found.");
    }

    rapidjson::Document d;
    d.SetObject();
    rapidjson::Document::AllocatorType& a = d.GetAllocator();
    d.AddMember("filename",  str(ref.filename,  a), a);
    d.AddMember("mime", str(ref.mime_type, a), a);

    // Text extracted from the original at upload (image/PDF/...) lives next to
    // it as <file_key>.txt; prefer it over bytes the model can't use. A missing
    // text object falls through to the branches below.
    if (!ref.text_key.empty()) {
        try {
            const std::string text = ctx.storage->get_object(ref.text_key);
            mirobody::platform::log_debug("mcp[read_file]: '%s' -> extracted text, %lu bytes (key=%s)",
                                          ref.filename.c_str(), (unsigned long)text.size(),
                                          ref.text_key.c_str());
            d.AddMember("content", str(text, a), a);
            return Result::ok(to_json(d));
        } catch (const mirobody::storage::StorageError&) {
            mirobody::platform::log_debug("mcp[read_file]: extracted text missing (key=%s); falling back",
                                          ref.text_key.c_str());
        }
    }

    if (is_text_mime(ref.mime_type)) {
        std::string bytes;
        try {
            bytes = ctx.storage->get_object(file_key);
        } catch (const mirobody::storage::StorageError& e) {
            mirobody::platform::log_debug("mcp[read_file]: get_object failed for key %s: %s", file_key.c_str(), e.what());
            return Result::error(std::string("Failed to read file: ") + e.what());
        }
        mirobody::platform::log_debug("mcp[read_file]: '%s' -> inline content, %lu bytes (mime=%s)",
                                      ref.filename.c_str(), (unsigned long)bytes.size(),
                                      ref.mime_type.c_str());
        d.AddMember("content", str(bytes, a), a);
    } else {
        // No extracted text and not plain text: hand back a temporary signed URL
        // instead of inlining bytes the model can't use (falls back to the
        // stored URL if signing fails).
        std::string url;
        try {
            url = ctx.storage->presigned_url(file_key, 3600);
        } catch (const mirobody::storage::StorageError&) {
            mirobody::platform::log_debug("mcp[read_file]: presign failed for key %s; using stored url", file_key.c_str());
            url = ref.url;
        }
        mirobody::platform::log_debug("mcp[read_file]: '%s' -> url (mime=%s, not inlined)",
                                      ref.filename.c_str(), ref.mime_type.c_str());
        d.AddMember("url", str(url, a), a);
        d.AddMember("note", "Binary file; not inlined. Fetch it from the url.", a);
    }

    return Result::ok(to_json(d));
}

//------------------------------------------------------------------------------

const Tool kReadFile = {
    "read_file",
    "Read one of your uploaded files by its file_key (from list_files). Text "
    "files are returned inline as `content`; other types return a temporary `url`.",
    true,                                                     // auth
    { Param("file_key", Type::String, Required,
            "The file_key of the file to read, as returned by list_files.") },
    &read_file,
};

}   // namespace

MIROBODY_REGISTER_TOOL(kReadFile);
