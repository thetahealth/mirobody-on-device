#include "user/email.hpp"

#include "client/curl_tls.hpp"
#include "client/http_client.hpp"
#include "platform/log.hpp"

#include <curl/curl.h>
#include <openssl/rand.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <random>

namespace mirobody { namespace user {

namespace {

//------------------------------------------------------------------------------
// Small helpers
//------------------------------------------------------------------------------

std::string to_lower_trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    std::string out;
    out.reserve(e - b);
    for (std::size_t i = b; i < e; ++i) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(s[i]))));
    }
    return out;
}

// Normalize + sanity-check an address. On success writes the trimmed/lowered
// form to *out and returns nullopt; otherwise returns an error message.
mirobody::optional<std::string> normalize_email(const std::string& in, std::string* out) {
    std::string lower = to_lower_trim(in);
    if (lower.empty() || lower.find('@') == std::string::npos) {
        return std::string("Invalid email address.");
    }
    *out = lower;
    return mirobody::nullopt;
}

bool all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

// Cryptographically-random 6-digit code, zero-padded, drawn uniformly over
// 000000..999999 (rejection sampling removes the modulo bias secrets.randbelow
// avoids). Falls back to a std::random_device draw only if RAND_bytes fails.
std::string generate_code() {
    const std::uint64_t range = 1000000u;
    const std::uint64_t span  = 0x100000000ULL;       // 2^32
    const std::uint64_t limit = span - (span % range); // largest unbiased ceiling

    std::uint32_t v = 0;
    for (;;) {
        unsigned char b[4] = {0, 0, 0, 0};
        if (RAND_bytes(b, sizeof(b)) != 1) {
            std::random_device rd;
            v = static_cast<std::uint32_t>(rd());
        } else {
            v = (static_cast<std::uint32_t>(b[0]) << 24) |
                (static_cast<std::uint32_t>(b[1]) << 16) |
                (static_cast<std::uint32_t>(b[2]) <<  8) |
                 static_cast<std::uint32_t>(b[3]);
        }
        if (v < limit) break;
    }

    char buf[7];
    std::snprintf(buf, sizeof(buf), "%06u", static_cast<unsigned>(v % range));
    return std::string(buf);
}

// "alice@x.com" + "login" -> "alice@x.com:login"; empty service -> bare email.
std::string scope_key(const std::string& email, const std::string& service) {
    return service.empty() ? email : email + ":" + service;
}

// The substring after the last '@' of a normalized address, e.g.
// "user7@demo" -> "demo". Empty when there's no '@' (shouldn't happen post
// normalize_email, which rejects addresses without one).
std::string domain_of(const std::string& email) {
    std::size_t at = email.rfind('@');
    return at == std::string::npos ? std::string() : email.substr(at + 1);
}

// The predefined code for `email`, preferring an exact address match and falling
// back to a domain-wide match. Returns nullopt when neither map covers it.
mirobody::optional<std::string> lookup_predefined(
    const std::unordered_map<std::string, std::string>& by_email,
    const std::unordered_map<std::string, std::string>& by_domain,
    const std::string& email) {
    std::unordered_map<std::string, std::string>::const_iterator eit = by_email.find(email);
    if (eit != by_email.end()) return eit->second;
    if (!by_domain.empty()) {
        std::unordered_map<std::string, std::string>::const_iterator dit =
            by_domain.find(domain_of(email));
        if (dit != by_domain.end()) return dit->second;
    }
    return mirobody::nullopt;
}

const char* kCodePrefix  = "mirobody:email:code:";
const char* kLimitPrefix = "mirobody:email:limit:";

// Current time as an RFC 5322 Date header value, e.g. "Tue, 30 May 2026
// 12:43:20 +0800". SMTP servers and spam filters expect a Date header.
std::string rfc5322_date() {
    std::time_t t = std::time(nullptr);
    std::tm tm;
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    // %a/%b are locale-dependent; the C locale (the default) yields the English
    // abbreviations the RFC wants.
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S %z", &tm);
    return std::string(buf);
}

//------------------------------------------------------------------------------
// Shared store/cooldown logic
//------------------------------------------------------------------------------

// Implements the common send()/verify() flow around a pluggable delivery
// transport: predefined-code short-circuit, send cooldown, code persistence
// with TTL, and verification. Subclasses supply deliver() and config_error().
class StoringEmailValidator : public EmailCodeValidator {
public:
    StoringEmailValidator(cache::Cache& cache, int sending_interval, int expires_in,
                          std::unordered_map<std::string, std::string> predefined,
                          std::unordered_map<std::string, std::string> predefined_domains)
        : cache_(cache),
          sending_interval_(sending_interval > 0 ? sending_interval : 60),
          expires_in_(expires_in > 0 ? expires_in : 10 * 60),
          predefined_(std::move(predefined)),
          predefined_domains_(std::move(predefined_domains)) {}

    mirobody::optional<std::string> send(
        const std::string& to_email, int expires_in, const std::string& service) override {
        std::string lower;
        mirobody::optional<std::string> err = normalize_email(to_email, &lower);
        if (err) return err;

        // Predefined addresses/domains never trigger a real send; verify() checks them.
        if (lookup_predefined(predefined_, predefined_domains_, lower)) return mirobody::nullopt;

        mirobody::optional<std::string> cfg = config_error();
        if (cfg) return cfg;

        const std::string key = scope_key(lower, service);

        // Honor the cooldown: a recent send leaves the limit key alive. Report
        // success so callers don't surface an error for benign rapid retries.
        if (cache_.exists(kLimitPrefix + key)) return mirobody::nullopt;

        const int ttl = (expires_in > 0) ? expires_in : expires_in_;
        const std::string code = generate_code();

        mirobody::optional<std::string> derr = deliver(lower, code);
        if (derr) return derr;

        cache_.set(kCodePrefix + key, code, std::chrono::seconds(ttl));
        cache_.set(kLimitPrefix + key, code, std::chrono::seconds(sending_interval_));
        return mirobody::nullopt;
    }

    mirobody::optional<std::string> verify(
        const std::string& to_email, const std::string& code, const std::string& service) override {
        std::string lower;
        mirobody::optional<std::string> err = normalize_email(to_email, &lower);
        if (err) return err;

        mirobody::optional<std::string> predef = lookup_predefined(predefined_, predefined_domains_, lower);
        if (predef) {
            return *predef == code ? mirobody::optional<std::string>()
                                   : mirobody::optional<std::string>(std::string("Invalid code."));
        }

        if (code.empty()) return std::string("Empty code.");
        if (!all_digits(code)) return std::string("Invalid code.");

        const std::string key = scope_key(lower, service);
        mirobody::optional<std::string> stored = cache_.get(kCodePrefix + key);
        if (stored && *stored == code) {
            // Single-use: consume the code on a successful match. (The Python
            // redis path left it until TTL; deleting is the safer behavior.)
            cache_.del(kCodePrefix + key);
            return mirobody::nullopt;
        }
        // A missing key here also covers natural TTL expiry, reported the same
        // as a wrong code so the response doesn't leak which case occurred.
        return std::string("Invalid code.");
    }

protected:
    // Deliver `code` to `to_email`. Returns nullopt on success, else an error.
    virtual mirobody::optional<std::string> deliver(
        const std::string& to_email, const std::string& code) = 0;

    // nullopt when the transport is usable; otherwise the error send() returns
    // before attempting delivery.
    virtual mirobody::optional<std::string> config_error() const = 0;

    cache::Cache& cache_;
    int sending_interval_;
    int expires_in_;
    std::unordered_map<std::string, std::string> predefined_;
    std::unordered_map<std::string, std::string> predefined_domains_;
};

//------------------------------------------------------------------------------
// SMTP transport (libcurl)
//------------------------------------------------------------------------------

struct UploadCtx {
    const std::string* data;
    std::size_t offset;
};

size_t smtp_read_cb(char* buffer, size_t size, size_t nitems, void* userp) {
    UploadCtx* ctx = static_cast<UploadCtx*>(userp);
    size_t room = size * nitems;
    size_t remaining = ctx->data->size() - ctx->offset;
    size_t n = remaining < room ? remaining : room;
    if (n) {
        std::memcpy(buffer, ctx->data->data() + ctx->offset, n);
        ctx->offset += n;
    }
    return n;
}

// Send a prebuilt RFC 5322 `message` to one recipient over SMTP(S). Transport
// only -- the caller composes the message. nullopt on success, else an error.
mirobody::optional<std::string> smtp_send(const std::string& host, int port,
                                          const std::string& user, const std::string& pass,
                                          const std::string& from_email, const std::string& to_email,
                                          const std::string& message) {
    CURL* curl = curl_easy_init();
    if (!curl) return std::string("curl_easy_init failed");

    const int  use_port     = port > 0 ? port : 465;
    const bool implicit_tls = (use_port == 465);
    const std::string url = (implicit_tls ? "smtps://" : "smtp://") +
                            host + ":" + std::to_string(use_port);
    const std::string mail_from = "<" + from_email + ">";
    const std::string rcpt      = "<" + to_email + ">";

    UploadCtx ctx;
    ctx.data = &message;
    ctx.offset = 0;

    struct curl_slist* recipients = curl_slist_append(nullptr, rcpt.c_str());

    client::configure_tls_trust(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERNAME, user.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, pass.c_str());
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, mail_from.c_str());
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
    if (!implicit_tls) {
        curl_easy_setopt(curl, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));
    }
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, smtp_read_cb);
    curl_easy_setopt(curl, CURLOPT_READDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(recipients);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        return std::string("Failed to send email: ") + curl_easy_strerror(rc);
    }
    return mirobody::nullopt;
}

// Build a minimal RFC 5322 HTML message (CRLF line endings as SMTP requires).
std::string build_rfc5322(const std::string& from_display, const std::string& to_email,
                          const std::string& subject, const std::string& html_body) {
    std::string msg;
    msg += "Date: " + rfc5322_date() + "\r\n";
    msg += "To: " + to_email + "\r\n";
    msg += "From: " + from_display + "\r\n";
    msg += "Subject: " + subject + "\r\n";
    msg += "MIME-Version: 1.0\r\n";
    msg += "Content-Type: text/html; charset=UTF-8\r\n";
    msg += "\r\n";
    msg += html_body;
    msg += "\r\n";
    return msg;
}

// Send a one-off HTML email via Mandrill's raw messages/send.json (no template).
mirobody::optional<std::string> mandrill_send_html(const std::string& api_key,
                                                   const std::string& from_email, const std::string& from_name,
                                                   const std::string& to_email, const std::string& subject,
                                                   const std::string& html_body) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("key");     w.String(api_key.c_str());
    w.Key("message"); w.StartObject();
        w.Key("html");       w.String(html_body.c_str(), static_cast<rapidjson::SizeType>(html_body.size()));
        w.Key("subject");    w.String(subject.c_str());
        w.Key("from_email"); w.String(from_email.c_str());
        w.Key("from_name");  w.String(from_name.c_str());
        w.Key("to"); w.StartArray(); w.StartObject();
            w.Key("email"); w.String(to_email.c_str());
            w.Key("type");  w.String("to");
        w.EndObject(); w.EndArray();
    w.EndObject();
    w.Key("async"); w.Bool(false);
    w.EndObject();

    client::HttpRequest req;
    req.url = "https://mandrillapp.com/api/1.0/messages/send.json";
    req.content_type = "application/json";
    req.body = std::string(sb.GetString(), sb.GetSize());

    client::HttpClient http;
    client::HttpResponse resp = http.post(req);
    if (resp.status < 200 || resp.status >= 300) {
        return std::string("Failed to send email to ") + to_email + ": " + resp.body;
    }
    rapidjson::Document doc;
    doc.Parse(resp.body.c_str(), resp.body.size());
    if (doc.HasParseError() || !doc.IsArray() || doc.Empty()) {
        return std::string("Failed to send email to ") + to_email + ": " + resp.body;
    }
    const rapidjson::Value& first = doc[0];
    const std::string status = (first.IsObject() && first.HasMember("status") && first["status"].IsString())
        ? std::string(first["status"].GetString(), first["status"].GetStringLength()) : std::string();
    if (status != "sent" && status != "queued") {
        return std::string("Failed to send email to ") + to_email + ": " + resp.body;
    }
    return mirobody::nullopt;
}

class SmtpEmailValidator : public StoringEmailValidator {
public:
    SmtpEmailValidator(cache::Cache& cache, std::string host, int port,
                       std::string user, std::string pass,
                       std::string from_email, std::string from_name,
                       int sending_interval, int expires_in,
                       std::unordered_map<std::string, std::string> predefined,
                       std::unordered_map<std::string, std::string> predefined_domains)
        : StoringEmailValidator(cache, sending_interval, expires_in,
                                std::move(predefined), std::move(predefined_domains)),
          host_(std::move(host)),
          port_(port > 0 ? port : 465),
          user_(std::move(user)),
          pass_(std::move(pass)),
          from_email_(std::move(from_email)),
          from_name_(std::move(from_name)) {}

protected:
    mirobody::optional<std::string> config_error() const override {
        if (host_.empty() || user_.empty() || pass_.empty()) {
            return std::string("Invalid SMTP configuration.");
        }
        return mirobody::nullopt;
    }

    mirobody::optional<std::string> deliver(
        const std::string& to_email, const std::string& code) override {
        mirobody::optional<std::string> err =
            smtp_send(host_, port_, user_, pass_, from_email_, to_email, build_message(to_email, code));
        if (err) return err;
        platform::log_debug("email: verification code sent to %s via SMTP", to_email.c_str());
        return mirobody::nullopt;
    }

private:
    // RFC 5322 message: headers, blank line, HTML body. CRLF line endings as
    // SMTP requires.
    std::string build_message(const std::string& to_email, const std::string& code) const {
        std::string from = from_name_.empty()
            ? from_email_
            : (from_name_ + " <" + from_email_ + ">");

        std::string spaced;            // "1 2 3 4 5 6"
        for (std::size_t i = 0; i < code.size(); ++i) {
            if (i) spaced.push_back(' ');
            spaced.push_back(code[i]);
        }

        std::string msg;
        msg += "Date: " + rfc5322_date() + "\r\n";
        msg += "To: " + to_email + "\r\n";
        msg += "From: " + from + "\r\n";
        msg += "Subject: Your Verification Code\r\n";
        msg += "MIME-Version: 1.0\r\n";
        msg += "Content-Type: text/html; charset=UTF-8\r\n";
        msg += "\r\n";
        msg +=
            "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"></head><body>"
            "<div style=\"max-width:600px;margin:0 auto;padding:20px;"
            "font-family:Arial,sans-serif;\">"
            "<h2 style=\"color:#333;\">Your Verification Code</h2>"
            "<p>Please use the following code to complete your verification:</p>"
            "<div style=\"text-align:center;margin:20px 0;\">"
            "<span style=\"font-size:32px;font-weight:bold;letter-spacing:8px;"
            "color:#333;\">" + spaced + "</span></div>"
            "<p style=\"color:#666;font-size:14px;\">This code will expire in 10 minutes.</p>"
            "<p style=\"color:#999;font-size:12px;\">If you did not request this code, "
            "please ignore this email.</p>"
            "</div></body></html>\r\n";
        return msg;
    }

    std::string host_;
    int         port_;
    std::string user_;
    std::string pass_;
    std::string from_email_;
    std::string from_name_;
};

//------------------------------------------------------------------------------
// Mandrill transport (HTTP API)
//------------------------------------------------------------------------------

class MandrillEmailValidator : public StoringEmailValidator {
public:
    MandrillEmailValidator(cache::Cache& cache, std::string api_key, std::string tmpl,
                           std::string from_email, std::string from_name,
                           int sending_interval, int expires_in,
                           std::unordered_map<std::string, std::string> predefined,
                           std::unordered_map<std::string, std::string> predefined_domains)
        : StoringEmailValidator(cache, sending_interval, expires_in,
                                std::move(predefined), std::move(predefined_domains)),
          api_key_(std::move(api_key)),
          template_(std::move(tmpl)),
          from_email_(std::move(from_email)),
          from_name_(std::move(from_name)) {}

protected:
    mirobody::optional<std::string> config_error() const override {
        if (api_key_.empty()) return std::string("Invalid email client.");
        return mirobody::nullopt;
    }

    mirobody::optional<std::string> deliver(
        const std::string& to_email, const std::string& code) override {
        std::string formatted;         // "<span>1</span><span>2</span>..."
        for (std::size_t i = 0; i < code.size(); ++i) {
            formatted += "<span>";
            formatted.push_back(code[i]);
            formatted += "</span>";
        }

        const std::string body = build_request(to_email, formatted);

        client::HttpRequest req;
        req.url = "https://mandrillapp.com/api/1.0/messages/send-template.json";
        req.content_type = "application/json";
        req.body = body;

        client::HttpClient http;
        client::HttpResponse resp = http.post(req);
        if (resp.status < 200 || resp.status >= 300) {
            return std::string("Failed to send email to ") + to_email + ": " + resp.body;
        }

        // Success body is a JSON array; the first entry's status must be sent or
        // queued. Anything else (e.g. "rejected", "invalid") is a failure.
        rapidjson::Document doc;
        doc.Parse(resp.body.c_str(), resp.body.size());
        if (doc.HasParseError() || !doc.IsArray() || doc.Empty()) {
            return std::string("Failed to send email to ") + to_email + ": " + resp.body;
        }
        const rapidjson::Value& first = doc[0];
        std::string status = (first.IsObject() && first.HasMember("status") && first["status"].IsString())
            ? std::string(first["status"].GetString(), first["status"].GetStringLength())
            : std::string();
        if (status != "sent" && status != "queued") {
            return std::string("Failed to send email to ") + to_email + ": " + resp.body;
        }
        platform::log_debug("email: verification code sent to %s via Mandrill", to_email.c_str());
        return mirobody::nullopt;
    }

private:
    std::string build_request(const std::string& to_email, const std::string& formatted_code) const {
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> w(sb);

        w.StartObject();
        w.Key("key");           w.String(api_key_.c_str());
        w.Key("template_name"); w.String(template_.c_str());

        w.Key("template_content");
        w.StartArray();
        w.StartObject();
        w.Key("name");    w.String("CODE");
        w.Key("content"); w.String(formatted_code.c_str());
        w.EndObject();
        w.EndArray();

        w.Key("message");
        w.StartObject();
        w.Key("subject");    w.String("Your Verification Code");
        w.Key("from_email"); w.String(from_email_.c_str());
        w.Key("from_name");  w.String(from_name_.c_str());
        w.Key("to");
        w.StartArray();
        w.StartObject();
        w.Key("email"); w.String(to_email.c_str());
        w.Key("type");  w.String("to");
        w.EndObject();
        w.EndArray();
        w.Key("merge_vars");
        w.StartArray();
        w.StartObject();
        w.Key("rcpt"); w.String(to_email.c_str());
        w.Key("vars");
        w.StartArray();
        w.StartObject();
        w.Key("name");    w.String("CODE");
        w.Key("content"); w.String(formatted_code.c_str());
        w.EndObject();
        w.EndArray();
        w.EndObject();
        w.EndArray();
        w.EndObject();   // message

        w.Key("async"); w.Bool(false);
        w.EndObject();

        return std::string(sb.GetString(), sb.GetSize());
    }

    std::string api_key_;
    std::string template_;
    std::string from_email_;
    std::string from_name_;
};

//------------------------------------------------------------------------------
// Dummy transport (no real delivery)
//------------------------------------------------------------------------------

// Fallback when nothing is configured. Real sends report the missing transport,
// but predefined addresses still verify (and "send" succeeds for them so demo
// logins flow without SMTP — a small, deliberate improvement over the Python
// Dummy, which returned an error from send() even for predefined addresses).
class DummyEmailValidator : public EmailCodeValidator {
public:
    DummyEmailValidator(std::unordered_map<std::string, std::string> predefined,
                        std::unordered_map<std::string, std::string> predefined_domains)
        : predefined_(std::move(predefined)),
          predefined_domains_(std::move(predefined_domains)) {}

    mirobody::optional<std::string> send(
        const std::string& to_email, int /*expires_in*/, const std::string& /*service*/) override {
        std::string lower = to_lower_trim(to_email);
        if (lookup_predefined(predefined_, predefined_domains_, lower)) return mirobody::nullopt;
        return std::string("No SMTP server configured.");
    }

    mirobody::optional<std::string> verify(
        const std::string& to_email, const std::string& code, const std::string& /*service*/) override {
        std::string lower = to_lower_trim(to_email);
        mirobody::optional<std::string> predef = lookup_predefined(predefined_, predefined_domains_, lower);
        if (predef && *predef == code) return mirobody::nullopt;
        return std::string("No SMTP server configured.");
    }

private:
    std::unordered_map<std::string, std::string> predefined_;
    std::unordered_map<std::string, std::string> predefined_domains_;
};

// Lowercase/trim every predefined key so lookups against a normalized address
// match regardless of how the config spelled it.
std::unordered_map<std::string, std::string> normalize_predefined(
    const std::unordered_map<std::string, std::string>& in) {
    std::unordered_map<std::string, std::string> out;
    out.reserve(in.size());
    for (std::unordered_map<std::string, std::string>::const_iterator it = in.begin(); it != in.end(); ++it) {
        out[to_lower_trim(it->first)] = it->second;
    }
    return out;
}

bool contains_ci(const std::string& haystack, const char* needle) {
    return to_lower_trim(haystack).find(needle) != std::string::npos;
}

}  // namespace

//------------------------------------------------------------------------------
// Factory
//------------------------------------------------------------------------------

std::unique_ptr<EmailCodeValidator> create_email_validator(
    const EmailValidatorOptions& opts, cache::Cache& cache) {
    std::unordered_map<std::string, std::string> predefined = normalize_predefined(opts.predefined_codes);
    std::unordered_map<std::string, std::string> predefined_domains = normalize_predefined(opts.predefined_domain_codes);
    const std::string from_name = opts.from_name.empty() ? std::string("Theta Wellness") : opts.from_name;

    // 1. Mandrill auto-detected from an smtp_host pointing at mandrillapp.com:
    //    the SMTP password doubles as the Mandrill API key.
    if (contains_ci(opts.smtp_host, "mandrillapp.com") &&
        !opts.smtp_pass.empty() && !opts.from_email.empty() && !opts.mandrill_template.empty()) {
        return std::unique_ptr<EmailCodeValidator>(new MandrillEmailValidator(
            cache, opts.smtp_pass, opts.mandrill_template, opts.from_email, from_name,
            opts.sending_interval, opts.expires_in, predefined, predefined_domains));
    }

    // 2. Direct SMTP.
    if (!opts.smtp_host.empty() && !opts.smtp_user.empty() &&
        !opts.smtp_pass.empty() && !opts.from_email.empty()) {
        return std::unique_ptr<EmailCodeValidator>(new SmtpEmailValidator(
            cache, opts.smtp_host, opts.smtp_port, opts.smtp_user, opts.smtp_pass,
            opts.from_email, from_name, opts.sending_interval, opts.expires_in,
            predefined, predefined_domains));
    }

    // 3. Mandrill via an explicit API key + template.
    if (!opts.mandrill_api_key.empty() && !opts.from_email.empty() && !opts.mandrill_template.empty()) {
        return std::unique_ptr<EmailCodeValidator>(new MandrillEmailValidator(
            cache, opts.mandrill_api_key, opts.mandrill_template, opts.from_email, from_name,
            opts.sending_interval, opts.expires_in, predefined, predefined_domains));
    }

    // 4. No transport configured.
    return std::unique_ptr<EmailCodeValidator>(new DummyEmailValidator(predefined, predefined_domains));
}

//------------------------------------------------------------------------------
// One-off transactional mail
//------------------------------------------------------------------------------

mirobody::optional<std::string> send_email(const EmailValidatorOptions& opts,
                                           const std::string& to_email,
                                           const std::string& subject,
                                           const std::string& html_body) {
    std::string to;
    mirobody::optional<std::string> err = normalize_email(to_email, &to);
    if (err) return err;

    const std::string from_name = opts.from_name.empty() ? std::string("Theta Wellness") : opts.from_name;

    // Same transport precedence as create_email_validator: direct SMTP first
    // (an smtp_host that isn't Mandrill, with user/pass/from), then Mandrill
    // (key from smtp_pass when smtp_host is mandrillapp.com, else mandrill_api_key).
    const bool is_mandrill_host = contains_ci(opts.smtp_host, "mandrillapp.com");
    if (!opts.smtp_host.empty() && !is_mandrill_host &&
        !opts.smtp_user.empty() && !opts.smtp_pass.empty() && !opts.from_email.empty()) {
        const std::string from_display = from_name + " <" + opts.from_email + ">";
        return smtp_send(opts.smtp_host, opts.smtp_port, opts.smtp_user, opts.smtp_pass,
                         opts.from_email, to, build_rfc5322(from_display, to, subject, html_body));
    }
    const std::string key = is_mandrill_host ? opts.smtp_pass : opts.mandrill_api_key;
    if (!key.empty() && !opts.from_email.empty()) {
        return mandrill_send_html(key, opts.from_email, from_name, to, subject, html_body);
    }
    return std::string("no email transport configured");
}

}}
