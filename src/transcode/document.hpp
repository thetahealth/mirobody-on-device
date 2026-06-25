#pragma once

// Document transcoder: turns an uploaded PDF or spreadsheet into an ordered
// list of LLM-consumable parts (text and/or images), the document counterpart
// to the image transcoder in image.hpp. Where image::Transcoder makes one image
// fit a vision model's input limits, document::Transcoder decomposes a richer
// container into parts a chat turn can carry.
//
// Strategy is "text first, image (and OCR) fallback":
//
//   - PDF      each page's embedded text layer is extracted as a text part.
//              A page with little/no extractable text (a scan) is instead
//              rasterized to an image, run through image::Transcoder so it
//              satisfies the target vision model, and emitted as an image part;
//              when OCR is compiled in (MIROBODY_ENABLE_OCR) the raster is also
//              OCR'd and the recovered text appended as a text part.
//   - .xlsx    each worksheet is rendered to a GitHub-flavored Markdown table
//   - .xls     (legacy BIFF) likewise, one Markdown table per sheet
//   - .csv     parsed (RFC 4180) and rendered to a single Markdown table
//
// Spreadsheet handling never produces image parts; it is always text.
//
// Build gating. The PDF path links PDFium and is compiled only when
// MIROBODY_ENABLE_PDF is defined; the OCR sub-step additionally requires
// MIROBODY_ENABLE_OCR (Tesseract + Leptonica). The legacy .xls path requires
// MIROBODY_ENABLE_XLS (vendored libxls). With a gate off, the corresponding
// input Format still sniffs, but process() throws DocumentError("...not built").
// CSV and .xlsx (xlnt) are always available.
//
// Threading. PDFium's library init/teardown is process-global and not
// thread-safe, and a PDFium document may not be touched concurrently. The PDF
// path serializes all PDFium work behind an internal mutex and owns the global
// init via a one-time guard, so Transcoder itself is safe to call from multiple
// threads; calls are simply not parallel while a PDF is being processed.

#include "compat/cxx11.hpp"
#include "transcode/image.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace mirobody { namespace document {

//------------------------------------------------------------------------------

// Thrown when the input cannot be decoded (unknown/unsupported format, corrupt
// data), or when the input's format was recognized but its handler was compiled
// out (see the build-gating note above).
class DocumentError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

//------------------------------------------------------------------------------

// Container format, as sniffed from the leading magic bytes.
enum class Format { Unknown, Pdf, Xlsx, Xls, Csv };

// The IANA media type for a format, or "" for Unknown.
const char* mime_type(Format f);

//------------------------------------------------------------------------------

// One piece of extracted content. A Document is an ordered sequence of these:
// text parts carry Markdown / plain UTF-8; image parts carry an already
// vision-compliant image::Transcoded (the output of image::Transcoder). `page`
// is the 1-based source page (PDF) or sheet (spreadsheet) the part came from.
struct Part {
    enum class Kind { Text, Image };

    Kind              kind = Kind::Text;
    std::string       text;     // populated when kind == Text (UTF-8 / Markdown)
    image::Transcoded image;    // populated when kind == Image
    int               page = 0; // 1-based page / sheet index, 0 if not applicable
};

//------------------------------------------------------------------------------

// Knobs for a transcode. Defaults target the common case (Qwen-VL vision limits
// for rasterized pages, OCR on where built in, no page cap).
struct Options {
    // Vision limits applied to any rasterized PDF page before it becomes an
    // image part. See image::QwenLimits / GeminiLimits / GptLimits.
    image::Limits image_limits = image::QwenLimits;

    int raster_dpi = 150;          // density used to rasterize a scanned PDF page

    // A PDF page whose extractable text layer has fewer than this many
    // characters is treated as a scan and sent down the image/OCR path.
    int pdf_text_threshold = 8;

    bool        ocr_enabled = true;   // honored only in a MIROBODY_ENABLE_OCR build
    std::string ocr_lang    = "eng";  // Tesseract language(s), e.g. "eng" / "eng+chi_sim"
    std::string ocr_datapath;         // dir holding <lang>.traineddata; "" => TESSDATA_PREFIX

    // Cap on pages/sheets processed (0 = no cap). Extra pages are dropped and a
    // trailing note is appended; nothing is silently lost.
    std::size_t max_pages = 0;
};

//------------------------------------------------------------------------------

struct Document {
    Format            format = Format::Unknown;
    std::vector<Part> parts;
};

//------------------------------------------------------------------------------

class Transcoder {
public:
    explicit Transcoder(Options opts = Options());

    // Decompose `input` into parts. Throws DocumentError if the input cannot be
    // decoded, or if its format's handler was compiled out.
    Document process(const std::string& input) const;

    // Render a Document to a single Markdown string: text parts inline, image
    // parts as a "![page N image](...)" placeholder line. A convenience for
    // callers (and the CLI) that want one blob rather than the part list.
    static std::string to_markdown(const Document& doc);

    // Sniff the container format from the leading magic bytes. No decode; cheap.
    // Anything not recognized as PDF / xlsx / xls falls back to Csv (the only
    // text-tabular format with no distinguishing magic).
    static Format detect_format(const std::string& input);

private:
    Options opts_;
};

}}
