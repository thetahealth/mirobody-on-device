// Standalone CLI for the document transcoder (src/transcode/document.*).
//
// Exercises the "text first, image fallback" decomposition of a PDF /
// spreadsheet / CSV into LLM-consumable parts, without the embedded server.
//
//   document info <in>                       # sniff format + byte size
//   document extract <in> [<out.md>]         # decompose -> Markdown (stdout if no <out>)
//   document extract <in> <out.md> [--target qwen|gemini|gpt] [--dpi N]
//                                  [--no-ocr] [--ocr-lang L] [--ocr-data DIR]
//                                  [--max-pages N]
//
// Image parts (rasterized scanned PDF pages) are written next to <out> as
// sidecar files "<out>.p<N>.<ext>" and referenced from the Markdown.
//
// Exit status is 0 on success, 1 on usage / I/O / decode errors.

#include "transcode/document.hpp"
#include "transcode/image.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {

const char* kUsage =
    "Usage:\n"
    "  document info <in>\n"
    "  document extract <in> [<out.md>] [--target qwen|gemini|gpt] [--dpi N]\n"
    "                        [--no-ocr] [--ocr-lang L] [--ocr-data DIR] [--max-pages N]\n"
    "\n"
    "  --target picks the vision preset used for rasterized PDF pages (default: qwen).\n"
    "  With no <out.md> the Markdown is printed to stdout (image sidecars are skipped).\n";

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool write_file(const std::string& path, const std::string& data) {
    std::ofstream f(path.c_str(), std::ios::binary);
    if (!f) return false;
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

const char* fmt_name(mirobody::document::Format f) {
    using mirobody::document::Format;
    switch (f) {
        case Format::Pdf:  return "pdf";
        case Format::Xlsx: return "xlsx";
        case Format::Xls:  return "xls";
        case Format::Csv:  return "csv";
        default:           return "unknown";
    }
}

// Map an image content-type to a file extension for sidecar naming.
const char* ext_for(const std::string& content_type) {
    if (content_type == "image/png")  return "png";
    if (content_type == "image/webp") return "webp";
    return "jpg";
}

}  // namespace

int main(int argc, char** argv) {
    using namespace mirobody::document;

    if (argc < 3) {
        std::fputs(kUsage, stderr);
        return 1;
    }
    const std::string cmd = argv[1];
    const std::string in_path = argv[2];

    std::string input;
    if (!read_file(in_path, input)) {
        std::fprintf(stderr, "document: cannot read '%s'\n", in_path.c_str());
        return 1;
    }

    try {
        if (cmd == "info") {
            Format f = Transcoder::detect_format(input);
            std::printf("file:   %s\n", in_path.c_str());
            std::printf("format: %s\n", fmt_name(f));
            std::printf("mime:   %s\n", mime_type(f));
            std::printf("bytes:  %zu\n", input.size());
            return 0;
        }

        if (cmd == "extract") {
            // argv[3] is the optional <out.md> if it is not a flag.
            std::string out_path;
            int first_flag = 3;
            if (argc >= 4 && std::strncmp(argv[3], "--", 2) != 0) {
                out_path = argv[3];
                first_flag = 4;
            }

            Options opts;
            for (int i = first_flag; i < argc; ++i) {
                std::string flag = argv[i];
                auto next = [&](const char* name) -> std::string {
                    if (i + 1 >= argc) { std::fprintf(stderr, "document: %s needs a value\n", name); std::exit(1); }
                    return argv[++i];
                };
                if (flag == "--target") {
                    std::string t = next("--target");
                    if (t == "qwen")        opts.image_limits = mirobody::image::QwenLimits;
                    else if (t == "gemini") opts.image_limits = mirobody::image::GeminiLimits;
                    else if (t == "gpt")    opts.image_limits = mirobody::image::GptLimits;
                    else { std::fprintf(stderr, "document: unknown --target '%s' (qwen|gemini|gpt)\n", t.c_str()); return 1; }
                } else if (flag == "--dpi")       { opts.raster_dpi = static_cast<int>(std::strtol(next("--dpi").c_str(), nullptr, 10)); }
                else if (flag == "--no-ocr")      { opts.ocr_enabled = false; }
                else if (flag == "--ocr-lang")    { opts.ocr_lang = next("--ocr-lang"); }
                else if (flag == "--ocr-data")    { opts.ocr_datapath = next("--ocr-data"); }
                else if (flag == "--max-pages")   { opts.max_pages = static_cast<std::size_t>(std::strtoull(next("--max-pages").c_str(), nullptr, 10)); }
                else { std::fprintf(stderr, "document: unknown flag '%s'\n", flag.c_str()); return 1; }
            }

            Transcoder tc(opts);
            Document doc = tc.process(input);

            // Write image-part sidecars and rewrite their placeholders to links.
            std::string md;
            int text_parts = 0, image_parts = 0;
            for (std::size_t i = 0; i < doc.parts.size(); ++i) {
                const Part& p = doc.parts[i];
                if (p.kind == Part::Kind::Image) {
                    ++image_parts;
                    if (!out_path.empty()) {
                        std::string side = out_path + ".p" + std::to_string(p.page) + "." + ext_for(p.image.content_type);
                        if (!write_file(side, p.image.bytes)) {
                            std::fprintf(stderr, "document: cannot write sidecar '%s'\n", side.c_str());
                            return 1;
                        }
                        md += "![page " + std::to_string(p.page) + " image](" + side + ")\n\n";
                    } else {
                        md += "![page " + std::to_string(p.page) + " image](" + p.image.content_type + ", " +
                              std::to_string(p.image.width) + "x" + std::to_string(p.image.height) + ")\n\n";
                    }
                } else {
                    ++text_parts;
                    md += p.text;
                    if (!p.text.empty() && p.text[p.text.size() - 1] != '\n') md += '\n';
                    md += '\n';
                }
            }

            if (out_path.empty()) {
                std::fputs(md.c_str(), stdout);
            } else if (!write_file(out_path, md)) {
                std::fprintf(stderr, "document: cannot write '%s'\n", out_path.c_str());
                return 1;
            }

            std::fprintf(stderr, "in:     %s (%zu bytes)\n", in_path.c_str(), input.size());
            std::fprintf(stderr, "format: %s\n", fmt_name(doc.format));
            std::fprintf(stderr, "parts:  %d text, %d image\n", text_parts, image_parts);
            if (!out_path.empty()) std::fprintf(stderr, "out:    %s\n", out_path.c_str());
            return 0;
        }

        std::fputs(kUsage, stderr);
        return 1;
    } catch (const DocumentError& e) {
        std::fprintf(stderr, "document: %s\n", e.what());
        return 1;
    }
}
