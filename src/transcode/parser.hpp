#pragma once

// File text extraction via a multimodal LLM.
//
// An uploaded file is often an image or a PDF the model can't read as text. A
// Parser hands the raw bytes to a vision-capable model and asks it to transcribe
// the text, so the chat agent (and MCP resources/read) can work with a prior
// upload. Extraction runs once, at upload time; the result is stored in object
// storage next to the original (as `<file_key>.trans`) and referenced by the
// file's FileRef::text_key (see transcode/file.hpp).
//
// Two backends are provided, selected by config (FILE_PARSER = "gemini" |
// "qwen"); each makes a single, self-contained HTTP call to its provider:
//   - gemini : Google AI Studio generateContent (x-goog-api-key auth), model
//              gemini-3.6-flash (override: FILE_PARSER_GEMINI_MODEL)
//   - qwen   : DashScope OpenAI-compatible chat/completions, model qwen3.7-plus
//              (override: FILE_PARSER_QWEN_MODEL). Must name a vision-capable
//              model: the request carries the file as an image part.
//
// Parsers are stateless and safe to share across threads (each call uses its own
// HTTP client); construct one at startup via make_parser and borrow it.

#include <cstddef>
#include <memory>
#include <string>

namespace mirobody {

struct Config;

namespace file {

class Parser {
public:
    virtual ~Parser() = default;

    // Extract the text from `bytes` (a file of `mime_type`, named `filename` for
    // logging only). Returns the extracted text, or "" on any failure (missing
    // credentials, transport error, empty result) -- failures are logged, never
    // thrown, so a bad extraction degrades to "no text" rather than failing the
    // upload. Blocking: makes a network round-trip.
    virtual std::string extract_text(const std::string& bytes,
                                     const std::string& mime_type,
                                     const std::string& filename) = 0;

    // Backend name for logging ("gemini" / "qwen").
    virtual const char* name() const = 0;
};

// Build the parser selected by config (the FILE_PARSER key; defaults to
// "gemini"). Returns nullptr when extraction is disabled (FILE_PARSER unset to
// "none"/"off", or an unknown value) -- the caller then simply skips extraction.
// A parser whose provider credentials are unset is still returned; it logs and
// yields "" at call time.
std::unique_ptr<Parser> make_parser(const Config& cfg);

//------------------------------------------------------------------------------

// Upper bound, in bytes, on the text of one user file that reaches a model, and
// the truncation enforcing it. Every Parser caps what it extracts, so a stored
// `<file_key>.trans` sidecar is already within the bound. A file whose bytes ARE
// its text (text/*, JSON, CSV, ...) skips extraction entirely, so a read path
// serving those bytes applies the cap itself -- otherwise a large text upload
// reaches a context the extracted kind cannot.
const std::size_t kMaxTextBytes = 100000;

// `s` truncated to kMaxTextBytes with "\n...[truncated]" appended, or unchanged
// when it already fits. The cut keeps whole UTF-8 sequences, and the marker lets
// a reader tell a cut document from a short one.
std::string cap_text(std::string s);

}}
