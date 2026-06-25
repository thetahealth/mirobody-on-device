#pragma once

#include "cache/cache.hpp"
#include "compat/cxx11.hpp"

#include <memory>
#include <string>
#include <unordered_map>

namespace mirobody { namespace user {

//------------------------------------------------------------------------------
// EmailCodeValidator
//------------------------------------------------------------------------------

// Sends and verifies short numeric verification codes delivered to an email
// address. Both calls return mirobody::nullopt on success, or a human-readable
// error message on failure — mirroring the Python `str | None` contract where
// `None` means "ok".
//
//   - `service` namespaces a code so one email can hold independent codes for,
//     say, "login" vs "delete-account". Empty groups them under the bare email.
//   - `expires_in` overrides the configured default code lifetime (in seconds)
//     for a single send; pass <= 0 to use the default.
//
// Issued codes and the per-email send cooldown are kept in a cache::Cache, which
// already abstracts the redis-vs-in-memory split the Python version hand-rolled.
// Cache is not thread-safe, so a validator is meant to be driven from a single
// thread (the server's lws service thread, where HTTP handlers run).
class EmailCodeValidator {
public:
    virtual ~EmailCodeValidator() {}

    virtual mirobody::optional<std::string> send(
        const std::string& to_email, int expires_in = 0, const std::string& service = "") = 0;

    virtual mirobody::optional<std::string> verify(
        const std::string& to_email, const std::string& code, const std::string& service = "") = 0;
};

//------------------------------------------------------------------------------
// Configuration / factory
//------------------------------------------------------------------------------

struct EmailValidatorOptions {
    // SMTP transport (preferred). Port 465 => implicit TLS (SMTPS); any other
    // port (e.g. 587) => plain connect upgraded with STARTTLS.
    std::string smtp_host;
    int         smtp_port = 0;        // 0 -> defaulted to 465 by the factory
    std::string smtp_user;
    std::string smtp_pass;

    // Mandrill HTTP API transport (fallback). `mandrill_template` is the name of
    // a template defined in the Mandrill account; it receives a `CODE` merge var.
    std::string mandrill_api_key;
    std::string mandrill_template;

    std::string from_email;
    std::string from_name;            // defaulted to "Theta Wellness" by the factory

    int sending_interval = 60;        // min seconds between sends to one address
    int expires_in       = 10 * 60;   // default code lifetime, seconds

    // Emails that bypass real delivery: verify() succeeds iff the supplied code
    // equals the mapped value. Mirrors the EMAIL_PREDEFINE_CODES config map.
    std::unordered_map<std::string, std::string> predefined_codes;

    // Domains whose every address bypasses real delivery, keyed by the part
    // after '@' (e.g. "demo", "symptom_entry_evaluation.com"). Consulted only
    // when an address isn't matched by predefined_codes. Mirrors the
    // EMAIL_PREDEFINE_DOMAIN_CODES config map.
    std::unordered_map<std::string, std::string> predefined_domain_codes;
};

// Build the validator that best fits `opts`, persisting issued codes and send
// cooldowns in `cache` (borrowed — it must outlive the validator; pass the
// server's Cache, redis- or memory-backed). Selection priority matches the
// Python factory:
//
//   1. Mandrill, when smtp_host contains "mandrillapp.com" (smtp_pass = API key)
//   2. SMTP,     when smtp_host + smtp_user + smtp_pass + from_email are all set
//   3. Mandrill, when mandrill_api_key + from_email + mandrill_template are set
//   4. Dummy     (predefined codes only; real sends report no transport)
std::unique_ptr<EmailCodeValidator> create_email_validator(
    const EmailValidatorOptions& opts, cache::Cache& cache);

}}
