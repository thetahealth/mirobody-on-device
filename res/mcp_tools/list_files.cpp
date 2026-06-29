// MCP tool: list_files. Authentication required.
//
// Lists the files the authenticated user has uploaded (the per-user index the
// chat upload path maintains in the cache; see transcode/). The resource side
// (resources/list) surfaces the same files to MCP-client UIs; this tool is the
// model-facing equivalent, so an agent mid-conversation can discover a prior
// upload (a lab report, a photo) and then fetch it with read_file.
//
// Reaches the cache through the ToolContext the dispatcher/service threads in;
// with no cache wired (ctx.cache null) it reports an empty list.

#include "mcp/tool.hpp"
#include "platform/log.hpp"
#include "transcode/file.hpp"

#include <rapidjson/document.h>

#include <vector>

namespace {

using namespace mirobody::mcp;

//------------------------------------------------------------------------------

Result list_files(const Args&, const UserInfo& user, const ToolContext& ctx) {
    if (!user.authed()) {
        return Result::error("Not authenticated");
    }

    std::vector<mirobody::file::FileRef> files;
    if (ctx.cache != nullptr) {
        files = mirobody::file::list(*ctx.cache, ctx.storage, user.user_id);
    }
    mirobody::platform::log_debug("mcp[list_files]: user %lld -> %lu file(s)%s",
                                  (long long)user.user_id, (unsigned long)files.size(),
                                  ctx.cache == nullptr ? " (no cache wired)" : "");

    rapidjson::Document d;
    d.SetArray();
    rapidjson::Document::AllocatorType& a = d.GetAllocator();
    for (std::size_t i = 0; i < files.size(); ++i) {
        const mirobody::file::FileRef& f = files[i];
        rapidjson::Value o(rapidjson::kObjectType);

        o.AddMember("filename",  rapidjson::Value(f.filename.c_str(),
                    static_cast<rapidjson::SizeType>(f.filename.size()), a), a);
        
        // Explicit transcript flag: without it a model sees an image/PDF MIME
        // type and concludes the file is unreadable instead of calling
        // read_file (which returns the extracted text as `content`).
        if (!f.text_key.empty()) {
            o.AddMember("file_key", rapidjson::Value(f.text_key.c_str(),
                        static_cast<rapidjson::SizeType>(f.text_key.size()), a), a);
            o.AddMember("mime", "text/plain", a);
        } else {
            o.AddMember("file_key", rapidjson::Value(f.file_key.c_str(),
                        static_cast<rapidjson::SizeType>(f.file_key.size()), a), a);
            o.AddMember("mime", rapidjson::Value(f.mime_type.c_str(),
                    static_cast<rapidjson::SizeType>(f.mime_type.size()), a), a);
        }

        if (f.uploaded_at > 0) {   // 0: entry predates the field
            const std::string ts = mirobody::file::iso8601_utc(f.uploaded_at);
            o.AddMember("uploaded_at", rapidjson::Value(ts.c_str(),
                        static_cast<rapidjson::SizeType>(ts.size()), a), a);
        }

        d.PushBack(o, a);
    }

    return Result::ok(to_json(d));
}

//------------------------------------------------------------------------------

const Tool kListFiles = {
    "list_files",
    "List the files you have uploaded. Returns each file's name, MIME type, "
    "file_key, upload time, and has_text -- true when text was extracted "
    "from the file (images, PDFs, scans), in which case read_file returns "
    "that text as `content` even though the file itself is not a text "
    "format. Pass a file_key to read_file to fetch a file's contents.",
    true,                                                     // auth
    {},                                                       // no parameters
    &list_files,
};

}   // namespace

MIROBODY_REGISTER_TOOL(kListFiles);
