// Standalone CLI for the image transcoder (src/transcode/image.*).
//
// Exercises decode -> smart-passthrough / resize -> re-encode end-to-end against
// the Aliyun Model Studio (Qwen-VL) input limits, without the embedded server.
//
//   image info <in>                 # sniff format + dimensions + byte size
//   image transcode <in> <out>      # make <in> compliant, write to <out>
//   image transcode <in> <out> --max-bytes 2000000 --max-pixels 1048576 \
//                               --max-aspect 200 --jpeg-quality 80
//
// Exit status is 0 on success, 1 on usage / I/O / decode errors.

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
    "  image info <in>\n"
    "  image transcode <in> <out> [--target qwen|gemini|gpt]\n"
    "                             [--max-bytes N] [--max-pixels N]\n"
    "                             [--max-aspect F] [--jpeg-quality Q]\n"
    "\n"
    "  --target picks a provider preset (default: qwen); the per-knob flags\n"
    "  override it regardless of order.\n";

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

const char* fmt_name(mirobody::image::Format f) {
    const char* m = mirobody::image::mime_type(f);
    return m[0] ? m : "unknown";
}

}  // namespace

int main(int argc, char** argv) {
    using namespace mirobody::image;

    if (argc < 3) {
        std::fputs(kUsage, stderr);
        return 1;
    }
    const std::string cmd = argv[1];
    const std::string in_path = argv[2];

    std::string input;
    if (!read_file(in_path, input)) {
        std::fprintf(stderr, "image: cannot read '%s'\n", in_path.c_str());
        return 1;
    }

    try {
        if (cmd == "info") {
            Format f = Transcoder::detect_format(input);
            std::printf("file:   %s\n", in_path.c_str());
            std::printf("format: %s\n", fmt_name(f));
            std::printf("bytes:  %zu\n", input.size());
            return 0;
        }

        if (cmd == "transcode") {
            if (argc < 4) { std::fputs(kUsage, stderr); return 1; }
            const std::string out_path = argv[3];

            // Pre-scan for --target so it sets the base preset before the
            // per-knob flags below override it, whatever the argument order.
            Limits limits;
            for (int i = 4; i + 1 < argc; i += 2) {
                if (std::string(argv[i]) == "--target") {
                    std::string t = argv[i + 1];
                    if (t == "qwen")        limits = QwenLimits;
                    else if (t == "gemini") limits = GeminiLimits;
                    else if (t == "gpt")    limits = GptLimits;
                    else { std::fprintf(stderr, "image: unknown --target '%s' (qwen|gemini|gpt)\n", t.c_str()); return 1; }
                }
            }
            for (int i = 4; i + 1 < argc; i += 2) {
                std::string flag = argv[i];
                std::string val = argv[i + 1];
                if (flag == "--target")           {}  // handled in the pre-scan
                else if (flag == "--max-bytes")   limits.max_bytes = std::strtoull(val.c_str(), nullptr, 10);
                else if (flag == "--max-pixels")  limits.max_pixels = std::strtoll(val.c_str(), nullptr, 10);
                else if (flag == "--max-aspect")  limits.max_aspect = std::strtod(val.c_str(), nullptr);
                else if (flag == "--jpeg-quality")limits.jpeg_quality = static_cast<int>(std::strtol(val.c_str(), nullptr, 10));
                else { std::fprintf(stderr, "image: unknown flag '%s'\n", flag.c_str()); return 1; }
            }

            Transcoder tc(limits);
            Transcoded t = tc.transcode(input);
            if (!write_file(out_path, t.bytes)) {
                std::fprintf(stderr, "image: cannot write '%s'\n", out_path.c_str());
                return 1;
            }
            std::printf("in:      %s (%zu bytes)\n", in_path.c_str(), input.size());
            std::printf("out:     %s (%zu bytes)\n", out_path.c_str(), t.bytes.size());
            std::printf("format:  %s\n", t.content_type.c_str());
            std::printf("size:    %dx%d\n", t.width, t.height);
            std::printf("changed: %s\n", t.changed ? "yes (re-encoded)" : "no (passthrough)");
            return 0;
        }

        std::fputs(kUsage, stderr);
        return 1;
    } catch (const ImageError& e) {
        std::fprintf(stderr, "image: %s\n", e.what());
        return 1;
    }
}
