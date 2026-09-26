#pragma once

// The HTTP request a handler receives, plus the request-parsing helpers that
// read it: query-string accessors (query_int / query_str / query_get), form-body
// accessors (form_str / form_int / form_file, over both
// application/x-www-form-urlencoded and multipart/form-data), and the underlying
// percent-decode / form-parse primitives as static methods. router.hpp includes
// this, so #include "server/router.hpp" still pulls it in.
//
// percent_decode and parse_form are public statics: they are general string
// transforms a few callers need on a raw string (a URL path, a form body) rather
// than through a Request. The rest of the parsing is a private implementation
// detail of the accessors.


#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mirobody { namespace server {

//------------------------------------------------------------------------------
// HTTP request
//------------------------------------------------------------------------------

class Request {
public:
    std::string method;
    std::string path;
    std::string query;
    std::string body;

    // Client metadata, populated by collect_request(). `ip` is the client
    // address: the leftmost X-Forwarded-For entry if present, else X-Real-IP,
    // else the direct peer address (may be empty if none can be resolved).
    // `user_agent` is the User-Agent header; `accept_language` is the client's
    // top preferred language tag (the first Accept-Language entry, e.g.
    // "en-US"). `timezone` is the IANA zone (e.g. "America/New_York") from the
    // non-standard X-Timezone header the frontend sets. All empty when absent.
    std::string ip;
    std::string user_agent;
    std::string accept_language;
    std::string timezone;

    // Raw value of the Authorization header (e.g. "Bearer <token>"), or empty
    // when absent. Populated by collect_request() in router.cpp.
    std::string authorization;

    // Value of the Host header (e.g. "api.example.com" or "localhost:8080"),
    // or empty when absent. Lets a handler construct an absolute URL pointing
    // back at this server. Populated by collect_request().
    std::string host;

    // Value of the Content-Type request header (e.g. "application/json" or
    // "multipart/form-data; boundary=..."), or empty when absent. Lets a
    // handler pick a body parser. Populated by collect_request().
    std::string content_type;

    // Raw value of the Accept-Encoding header (e.g. "gzip, deflate, br"), or
    // empty when absent. The router consults it to decide whether to gzip a
    // large fixed-length response body. Populated by collect_request().
    std::string accept_encoding;

    // The last SSE event id the client saw, naming where a resumed stream should
    // pick up. Read from the X-Last-Event-Id header, falling back to a
    // `last_event_id` query parameter; empty on a fresh connection.
    std::string last_event_id;

    // Names the chat session the request belongs to. Read from the X-Session-Id
    // header, falling back to a `session_id` query parameter; a handler may also
    // accept it from the request body. Empty when none is supplied.
    std::string session_id;

    // Authenticated identity, populated by the auth middleware from the JWT
    // claims (`sub` / `email`) and read by downstream handlers. user_id is the
    // decoded raw row id; 0 (and empty email) on unauthenticated routes.
    // Mutable so the auth layer can set them through the const reference that
    // handlers receive.
    mutable std::int64_t user_id = 0;
    mutable std::string  email;

    // Path parameters captured from a route registered with `{name}` segments,
    // e.g. registering "/mcp/{secret}" and receiving "/mcp/abc" yields
    // {"secret": "abc"}. Empty for exact-match routes. Mutable for the same
    // reason as above: the router fills it through the const handler reference.
    mutable std::unordered_map<std::string, std::string> path_params;

    //--------------------------------------------------------------------------
    // Query-string accessors (over `query`)
    //--------------------------------------------------------------------------

    // Integer parameter (e.g. "page" from "page=2&page_size=20"), or `fallback`
    // when the key is absent or its value doesn't parse.
    int query_int(const char* key, int fallback) const {
        std::string v;
        if (!query_find(query, key, v)) return fallback;
        const char* start = v.c_str();
        char* end = nullptr;
        const long n = std::strtol(start, &end, 10);
        return (end != start) ? static_cast<int>(n) : fallback;
    }

    // Raw (undecoded) string parameter, or `fallback` when absent. Verbatim --
    // for short enum-like / URL-safe tokens such as sort=name, order=asc, a JWT.
    std::string query_str(const char* key, const char* fallback) const {
        std::string v;
        return query_find(query, key, v) ? v : std::string(fallback);
    }

    // Percent-decoded string parameter, or "" when absent. For values that can
    // carry reserved/escaped characters (timestamps, ids).
    std::string query_get(const char* key) const {
        std::string v;
        return query_find(query, key, v) ? percent_decode(v) : std::string();
    }

    //--------------------------------------------------------------------------
    // Form-body accessors (over `body`, by `content_type`)
    //--------------------------------------------------------------------------

    // One uploaded file part of a multipart/form-data body.
    struct FilePart {
        std::string field_name;
        std::string filename;
        std::string content_type;
        std::string data;
    };

    // True when the body is a form this class can parse: either
    // application/x-www-form-urlencoded or multipart/form-data.
    bool is_form_body() const {
        const std::string ct = ascii_lower(content_type);
        return ct.find("multipart/form-data") != std::string::npos ||
               ct.find("application/x-www-form-urlencoded") != std::string::npos;
    }

    // Decoded value of a form field, or `fallback` when absent. (Form values are
    // already decoded during parsing, unlike query_str.)
    std::string form_str(const char* key, const char* fallback = "") const {
        ensure_form();
        for (std::size_t i = 0; i < form_fields_.size(); ++i) {
            if (form_fields_[i].first == key) return form_fields_[i].second;
        }
        return std::string(fallback);
    }

    // Integer form field, or `fallback` when absent / unparseable.
    int form_int(const char* key, int fallback) const {
        const std::string v = form_str(key);
        if (v.empty()) return fallback;
        const char* start = v.c_str();
        char* end = nullptr;
        const long n = std::strtol(start, &end, 10);
        return (end != start) ? static_cast<int>(n) : fallback;
    }

    // The first uploaded file with field name `key`, or nullptr when absent.
    const FilePart* form_file(const char* key) const {
        ensure_form();
        for (std::size_t i = 0; i < form_files_.size(); ++i) {
            if (form_files_[i].field_name == key) return &form_files_[i];
        }
        return nullptr;
    }

    // All uploaded file parts. A non-const reference into the (lazily parsed)
    // cache, so a caller can std::move the bytes out rather than copy them.
    std::vector<FilePart>& form_files() const {
        ensure_form();
        return form_files_;
    }

    //--------------------------------------------------------------------------
    // URL helpers (general string transforms; usable without a Request)
    //--------------------------------------------------------------------------

    // Percent-decode an application/x-www-form-urlencoded component: "%XX" ->
    // byte, and (when `plus_as_space`) '+' -> space. A malformed escape -- a lone
    // '%' or one not followed by two hex digits -- is left untouched. Pass
    // plus_as_space=false for a URL *path*, where '+' is a literal character.
    static std::string percent_decode(const std::string& s, bool plus_as_space = true) {
        std::string out;
        out.reserve(s.size());
        for (std::size_t i = 0; i < s.size(); ++i) {
            const char c = s[i];
            if (plus_as_space && c == '+') { out.push_back(' '); continue; }
            if (c == '%' && i + 2 < s.size()) {
                const int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
                if (hi >= 0 && lo >= 0) {
                    out.push_back(static_cast<char>((hi << 4) | lo));
                    i += 2;
                    continue;
                }
            }
            out.push_back(c);
        }
        return out;
    }

    // Parse an application/x-www-form-urlencoded body or a URL query string into
    // a flat map; keys and values are percent-decoded, last value wins on a
    // duplicate key. For callers that read many keys from one string.
    static std::map<std::string, std::string> parse_form(const std::string& s) {
        std::map<std::string, std::string> out;
        std::size_t i = 0;
        while (i < s.size()) {
            std::size_t amp = s.find('&', i);
            if (amp == std::string::npos) amp = s.size();
            const std::string pair = s.substr(i, amp - i);
            const std::size_t eq = pair.find('=');
            if (eq == std::string::npos) {
                if (!pair.empty()) out[percent_decode(pair)] = std::string();
            } else {
                out[percent_decode(pair.substr(0, eq))] = percent_decode(pair.substr(eq + 1));
            }
            i = amp + 1;
        }
        return out;
    }

private:
    //-- Shared primitives -----------------------------------------------------

    static int hex(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    // Raw (still percent-encoded) value of `key` in a "a=b&c=d" query string.
    // Returns true and sets `out` (possibly empty) on the first match; false when
    // absent. A bare `key` with no '=' matches with out = "".
    static bool query_find(const std::string& query, const std::string& key, std::string& out) {
        std::size_t i = 0;
        while (i < query.size()) {
            const std::size_t amp = query.find('&', i);
            const std::string  pair = query.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
            const std::size_t  eq = pair.find('=');
            const std::string  k = (eq == std::string::npos) ? pair : pair.substr(0, eq);
            if (k == key) {
                out = (eq == std::string::npos) ? std::string() : pair.substr(eq + 1);
                return true;
            }
            if (amp == std::string::npos) break;
            i = amp + 1;
        }
        return false;
    }

    static std::string ascii_lower(const std::string& s) {
        std::string out(s);
        for (std::size_t i = 0; i < out.size(); ++i) {
            char& c = out[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return out;
    }

    static std::string trim_ws(const std::string& s) {
        const std::size_t b = s.find_first_not_of(" \t");
        if (b == std::string::npos) return std::string();
        const std::size_t e = s.find_last_not_of(" \t");
        return s.substr(b, e - b + 1);
    }

    //-- Lazy form-body parse (cached; the body can be large) ------------------

    void ensure_form() const {
        if (form_parsed_) return;
        form_parsed_ = true;
        const std::string ct = ascii_lower(content_type);
        if (ct.find("multipart/form-data") != std::string::npos) {
            parse_multipart(body, content_type, form_fields_, form_files_);
        } else if (ct.find("application/x-www-form-urlencoded") != std::string::npos) {
            parse_urlencoded(body, form_fields_);
        }
        // Any other content type: not a form -- leave the caches empty.
    }

    typedef std::vector<std::pair<std::string, std::string> > Fields;

    // Parse "a=1&b=two&c" into decoded key/value pairs. A key with no '=' yields
    // an empty value; an empty key is dropped.
    static void parse_urlencoded(const std::string& body, Fields& fields) {
        std::size_t i = 0;
        while (i < body.size()) {
            const std::size_t amp = body.find('&', i);
            const std::size_t end = (amp == std::string::npos) ? body.size() : amp;
            const std::size_t eq  = body.find('=', i);
            std::string key, val;
            if (eq != std::string::npos && eq < end) {
                key = body.substr(i, eq - i);
                val = body.substr(eq + 1, end - eq - 1);
            } else {
                key = body.substr(i, end - i);
            }
            if (!key.empty()) {
                fields.push_back(std::make_pair(percent_decode(key), percent_decode(val)));
            }
            if (amp == std::string::npos) break;
            i = amp + 1;
        }
    }

    // boundary from "multipart/form-data; boundary=----X" -> "----X" (quotes
    // stripped); "" when absent.
    static std::string multipart_boundary(const std::string& content_type) {
        const std::size_t b = ascii_lower(content_type).find("boundary=");
        if (b == std::string::npos) return std::string();
        std::string val = trim_ws(content_type.substr(b + 9));
        const std::size_t semi = val.find(';');
        if (semi != std::string::npos) val = trim_ws(val.substr(0, semi));
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            val = val.substr(1, val.size() - 2);
        }
        return val;
    }

    // Read a `key="value"` (or bare `key=value`) parameter from a header line.
    static std::string header_param(const std::string& line, const char* key) {
        const std::string needle = std::string(key) + "=";
        const std::size_t p = ascii_lower(line).find(needle);
        if (p == std::string::npos) return std::string();
        const std::size_t v = p + needle.size();
        if (v < line.size() && line[v] == '"') {
            const std::size_t e = line.find('"', v + 1);
            if (e == std::string::npos) return std::string();
            return line.substr(v + 1, e - v - 1);
        }
        const std::size_t e = line.find_first_of("; \t", v);
        return line.substr(v, e == std::string::npos ? std::string::npos : e - v);
    }

    // Pull name / filename / Content-Type out of one part's CRLF-separated header
    // block (the bytes before the blank line that opens the part body).
    static void parse_part_headers(const std::string& headers, std::string& name,
                                   std::string& filename, std::string& content_type) {
        std::size_t i = 0;
        while (i < headers.size()) {
            const std::size_t eol = headers.find("\r\n", i);
            const std::string  line = headers.substr(
                i, eol == std::string::npos ? std::string::npos : eol - i);
            const std::string  low = ascii_lower(line);
            if (low.compare(0, 20, "content-disposition:") == 0) {
                name     = header_param(line, "name");
                filename = header_param(line, "filename");
            } else if (low.compare(0, 13, "content-type:") == 0) {
                content_type = trim_ws(line.substr(13));
            }
            if (eol == std::string::npos) break;
            i = eol + 2;
        }
    }

    // Parse a multipart/form-data body into text fields and file parts. A part
    // with a filename is a file; one with only a name is a text field. Malformed
    // input yields whatever parsed before the break (the caller validates).
    static void parse_multipart(const std::string& body, const std::string& content_type,
                                Fields& fields, std::vector<FilePart>& files) {
        const std::string boundary = multipart_boundary(content_type);
        if (boundary.empty()) return;
        const std::string delim = "--" + boundary;

        std::size_t pos = body.find(delim);
        if (pos == std::string::npos) return;
        pos += delim.size();

        while (pos <= body.size()) {
            if (body.compare(pos, 2, "--") == 0) break;       // closing "--boundary--"
            if (body.compare(pos, 2, "\r\n") == 0) pos += 2;  // CRLF after the boundary

            const std::size_t hdr_end = body.find("\r\n\r\n", pos);
            if (hdr_end == std::string::npos) break;
            const std::string headers = body.substr(pos, hdr_end - pos);
            const std::size_t content_begin = hdr_end + 4;

            // The part body runs up to the CRLF that precedes the next boundary.
            const std::size_t next = body.find("\r\n" + delim, content_begin);
            if (next == std::string::npos) break;
            std::string content = body.substr(content_begin, next - content_begin);

            std::string name, filename, part_ct;
            parse_part_headers(headers, name, filename, part_ct);

            if (!filename.empty()) {
                FilePart fp;
                fp.field_name   = name;
                fp.filename     = filename;
                fp.content_type = part_ct.empty() ? "application/octet-stream" : part_ct;
                fp.data         = std::move(content);
                files.push_back(std::move(fp));
            } else if (!name.empty()) {
                fields.push_back(std::make_pair(name, std::move(content)));
            }

            pos = next + 2 + delim.size();  // step past the CRLF and the next delim
        }
    }

    // Lazily-parsed form body cache (parsed once, on the first form_* access).
    mutable bool                  form_parsed_ = false;
    mutable Fields                form_fields_;
    mutable std::vector<FilePart> form_files_;
};

}
}
