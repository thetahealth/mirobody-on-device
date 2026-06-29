#pragma once

// Image transcoder that makes an arbitrary uploaded image conform to the input
// limits of a vision model's API, so it can be sent without being rejected or
// silently degraded. Three targets ship as ready-made Limits instances —
// QwenLimits / GeminiLimits / GptLimits (see below); QwenLimits is the default.
//
// Alibaba Cloud Model Studio (Qwen-VL), via the OpenAI-compatible / DashScope
// APIs — QwenLimits (the default):
//   - single image <= 10 MB
//   - each side > 10 px
//   - aspect ratio (long/short) <= 200:1
//   - output is always one of JPEG / PNG / WebP (the formats the API accepts)
// The model further downsamples by a token budget; we additionally cap the
// total pixel count so we never ship a needlessly large image.
//
// Gemini API — GeminiLimits: looser. Gemini tiles images (768x768 = 258 tokens)
// rather than rejecting on size, imposes no per-side minimum and no aspect-ratio
// limit, and accepts PNG/JPEG/WebP/HEIC/HEIF. The binding constraint is the
// 20 MB total inline-request cap, so the preset keeps the base64 payload under
// it; min-side and aspect checks are disabled.
//
// OpenAI GPT vision — GptLimits: looser still. GPT scales every image into a
// 2048x2048 box (no point sending more), has no per-side minimum and no aspect
// limit, accepts PNG/JPEG/WebP/non-animated GIF, and caps a single image at
// 20 MB. min-side and aspect checks are disabled.
//
// Strategy is "smart passthrough": if the input already satisfies every limit
// and is a JPEG/PNG/WebP, its bytes are returned untouched; otherwise the image
// is decoded, resized to fit, and re-encoded.
//
// Decodes JPEG, PNG, WebP and TIFF. BMP / GIF / HEIC inputs are NOT decodable
// with this codec stack (HEIC would need libheif); they raise ImageError. EXIF
// orientation is not auto-applied.

#include "compat/cxx11.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>

namespace mirobody { namespace image {

//------------------------------------------------------------------------------

// Thrown when the input cannot be decoded (unknown/unsupported format, corrupt
// data) or a codec fails mid-transcode.
class ImageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

//------------------------------------------------------------------------------

// Image container format, as sniffed from the leading magic bytes.
enum class Format { Unknown, Jpeg, Png, WebP, Tiff };

// The IANA media type for a format ("image/jpeg", ...), or "" for Unknown.
const char* mime_type(Format f);

//------------------------------------------------------------------------------

// The input limits the transcoder targets. The defaults match the documented
// Qwen-VL values; tweak a copy to change them (e.g. a smaller max_pixels to cut
// token cost, or a tighter max_bytes). To disable the per-side or aspect check,
// set min_side = 1 / max_aspect to a large value (see GeminiLimits / GptLimits).
struct Limits {
    std::size_t max_bytes    = 10u * 1024 * 1024;  // single image <= 10 MB
    int         min_side     = 11;                 // each side must be > 10 px
    long long   max_pixels   = 2560LL * 2560;      // total-pixel budget
    double      max_aspect   = 200.0;              // long/short <= 200:1
    int         jpeg_quality = 85;                 // re-encode quality (1..100)
};

//------------------------------------------------------------------------------

// Ready-made presets, one per target API. Pass one to the Transcoder
// constructor, or copy and tweak a field. Defined in image.cpp.
extern const Limits QwenLimits;    // Alibaba Cloud Model Studio (Qwen-VL); == the defaults
extern const Limits GeminiLimits;  // Gemini API (~13 MB payload, no min-side / aspect limit)
extern const Limits GptLimits;     // OpenAI GPT vision (20 MB, 2048px budget, no min-side / aspect limit)

//------------------------------------------------------------------------------

// The result of transcode(). When `changed` is false the input was already
// compliant and `bytes` aliases the original content verbatim.
struct Transcoded {
    std::string bytes;                     // the compliant image bytes
    std::string content_type;              // "image/jpeg" | "image/png" | "image/webp"
    int         width  = 0;
    int         height = 0;
    Format      format = Format::Unknown;  // the OUTPUT format
    bool        changed = false;           // false => passthrough (bytes == input)
};

//------------------------------------------------------------------------------

class Transcoder {
public:
    explicit Transcoder(Limits limits = Limits());

    // Make `input` conform to the configured limits. Returns the (possibly
    // unchanged) compliant bytes plus metadata. Throws ImageError if the input
    // cannot be decoded or no compliant encoding fits within max_bytes.
    Transcoded transcode(const std::string& input) const;

    // transcode() then wrap the result as a "data:<mime>;base64,<...>" URL,
    // ready to drop into an OpenAI/Qwen `image_url` message part.
    std::string to_data_url(const std::string& input) const;

    // Sniff the container format from the leading magic bytes. No decode; cheap.
    static Format detect_format(const std::string& input);

private:
    Limits limits_;
};

}}
