#include "transcode/image.hpp"

#include <catch2/catch_test_macros.hpp>

// The codecs are linked PUBLIC into mirobody_core, so the test target inherits
// them; we use libpng/libjpeg directly to synthesize valid inputs in memory.
#include <cstdio>
#include <jpeglib.h>
#include <png.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using mirobody::image::Format;
using mirobody::image::ImageError;
using mirobody::image::Limits;
using mirobody::image::Transcoder;
using mirobody::image::Transcoded;
using mirobody::image::QwenLimits;
using mirobody::image::GeminiLimits;
using mirobody::image::GptLimits;

namespace {

//------------------------------------------------------------------------------
// Synthetic-image builders + helpers
//------------------------------------------------------------------------------

// A PNG with a smooth (compressible) gradient. `alpha` selects an RGBA image
// with a horizontal alpha ramp; otherwise an opaque RGB image.
std::string make_png(int w, int h, bool alpha) {
    const int ch = alpha ? 4 : 3;
    std::vector<png_byte> px(static_cast<std::size_t>(w) * h * ch);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            png_byte* p = &px[(static_cast<std::size_t>(y) * w + x) * ch];
            p[0] = static_cast<png_byte>(x % 256);
            p[1] = static_cast<png_byte>(y % 256);
            p[2] = static_cast<png_byte>((x + y) % 256);
            if (alpha) p[3] = static_cast<png_byte>(w > 1 ? x * 255 / (w - 1) : 255);
        }
    }
    png_image image;
    std::memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    image.width = static_cast<png_uint_32>(w);
    image.height = static_cast<png_uint_32>(h);
    image.format = alpha ? PNG_FORMAT_RGBA : PNG_FORMAT_RGB;

    png_alloc_size_t size = 0;
    REQUIRE(png_image_write_to_memory(&image, nullptr, &size, 0, px.data(), 0, nullptr));
    std::string out(static_cast<std::size_t>(size), '\0');
    REQUIRE(png_image_write_to_memory(&image, &out[0], &size, 0, px.data(), 0, nullptr));
    out.resize(static_cast<std::size_t>(size));
    return out;
}

// A baseline JPEG with a smooth gradient.
std::string make_jpeg(int w, int h, int quality) {
    jpeg_compress_struct cinfo;
    jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    unsigned char* buf = nullptr;
    unsigned long  buf_size = 0;
    jpeg_mem_dest(&cinfo, &buf, &buf_size);
    cinfo.image_width = static_cast<JDIMENSION>(w);
    cinfo.image_height = static_cast<JDIMENSION>(h);
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    std::vector<std::uint8_t> row(static_cast<std::size_t>(w) * 3);
    while (cinfo.next_scanline < cinfo.image_height) {
        int y = static_cast<int>(cinfo.next_scanline);
        for (int x = 0; x < w; ++x) {
            row[x * 3 + 0] = static_cast<std::uint8_t>(x % 256);
            row[x * 3 + 1] = static_cast<std::uint8_t>(y % 256);
            row[x * 3 + 2] = static_cast<std::uint8_t>((x + y) % 256);
        }
        std::uint8_t* rp = row.data();
        jpeg_write_scanlines(&cinfo, &rp, 1);
    }
    jpeg_finish_compress(&cinfo);
    std::string out(reinterpret_cast<char*>(buf), buf_size);
    jpeg_destroy_compress(&cinfo);
    if (buf) free(buf);
    return out;
}

// True if the PNG `bytes` decode to an image carrying at least one non-opaque
// pixel — i.e. transparency survived the transcode.
bool png_has_transparency(const std::string& bytes) {
    png_image image;
    std::memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, bytes.data(), bytes.size())) return false;
    image.format = PNG_FORMAT_RGBA;
    std::vector<std::uint8_t> px(PNG_IMAGE_SIZE(image));
    if (!png_image_finish_read(&image, nullptr, px.data(), 0, nullptr)) {
        png_image_free(&image);
        return false;
    }
    png_image_free(&image);
    for (std::size_t i = 3; i < px.size(); i += 4) if (px[i] != 255) return true;
    return false;
}

std::string b64_decode(const std::string& s) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    std::uint32_t n = 0;
    int bits = 0;
    for (char c : s) {
        if (c == '=') break;
        int v = val(c);
        if (v < 0) continue;
        n = (n << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(static_cast<char>((n >> bits) & 0xFF)); }
    }
    return out;
}

double aspect_of(int w, int h) {
    return static_cast<double>(std::max(w, h)) / std::min(w, h);
}

}  // namespace

//------------------------------------------------------------------------------

TEST_CASE("detect_format sniffs magic bytes", "[image]") {
    REQUIRE(Transcoder::detect_format(make_png(8, 8, false)) == Format::Png);
    REQUIRE(Transcoder::detect_format(make_jpeg(8, 8, 80)) == Format::Jpeg);

    // "RIFF" + 4-byte size + "WEBP" + chunk fourcc (filler avoids embedded NULs
    // that would truncate the const char* constructor).
    std::string webp = "RIFFsizeWEBPVP8 ";
    REQUIRE(Transcoder::detect_format(webp) == Format::WebP);

    std::string tiff_le("II\x2a\x00", 4);
    std::string tiff_be("MM\x00\x2a", 4);
    REQUIRE(Transcoder::detect_format(tiff_le) == Format::Tiff);
    REQUIRE(Transcoder::detect_format(tiff_be) == Format::Tiff);

    REQUIRE(Transcoder::detect_format("not an image at all") == Format::Unknown);
    REQUIRE(Transcoder::detect_format("") == Format::Unknown);
}

//------------------------------------------------------------------------------

TEST_CASE("compliant image passes through untouched", "[image]") {
    std::string jpg = make_jpeg(100, 100, 85);
    Transcoded t = Transcoder().transcode(jpg);

    REQUIRE_FALSE(t.changed);
    REQUIRE(t.format == Format::Jpeg);
    REQUIRE(t.content_type == "image/jpeg");
    REQUIRE(t.width == 100);
    REQUIRE(t.height == 100);
    REQUIRE(t.bytes == jpg);  // verbatim
}

//------------------------------------------------------------------------------

TEST_CASE("oversized image is downscaled under the pixel budget", "[image]") {
    Limits lim;
    lim.max_pixels = 200 * 200;            // tight budget to force a downscale
    std::string png = make_png(1000, 1000, false);

    Transcoded t = Transcoder(lim).transcode(png);

    REQUIRE(t.changed);
    REQUIRE(t.format == Format::Jpeg);     // no alpha -> JPEG
    REQUIRE(static_cast<long long>(t.width) * t.height <= lim.max_pixels);

    // Idempotent: the now-compliant output passes through unchanged.
    Transcoded again = Transcoder(lim).transcode(t.bytes);
    REQUIRE_FALSE(again.changed);
}

//------------------------------------------------------------------------------

TEST_CASE("output is squeezed under the byte budget", "[image]") {
    Limits lim;
    lim.max_pixels = 200 * 200;            // force the re-encode path
    lim.max_bytes  = 4000;                 // 4 KB ceiling the encoder must hit
    std::string png = make_png(1000, 1000, false);

    Transcoded t = Transcoder(lim).transcode(png);

    REQUIRE(t.changed);
    REQUIRE(t.bytes.size() <= lim.max_bytes);
}

//------------------------------------------------------------------------------

TEST_CASE("extreme aspect ratio is clamped by padding", "[image]") {
    std::string strip = make_png(4000, 10, false);
    Transcoded t = Transcoder().transcode(strip);

    REQUIRE(t.changed);
    REQUIRE(std::min(t.width, t.height) >= 11);          // min-side floor (> 10 px)
    REQUIRE(aspect_of(t.width, t.height) <= 200.0 + 1e-6);
}

//------------------------------------------------------------------------------

TEST_CASE("transparency is preserved as PNG on re-encode", "[image]") {
    Limits lim;
    lim.max_pixels = 100 * 100;            // force a re-encode of a small alpha image
    std::string png = make_png(300, 300, true);

    Transcoded t = Transcoder(lim).transcode(png);

    REQUIRE(t.changed);
    REQUIRE(t.format == Format::Png);
    REQUIRE(t.content_type == "image/png");
    REQUIRE(png_has_transparency(t.bytes));
}

//------------------------------------------------------------------------------

TEST_CASE("undecodable input throws", "[image]") {
    REQUIRE_THROWS_AS(Transcoder().transcode("definitely not an image"), ImageError);

    // A PNG signature with a corrupt body, forced down the decode path by a
    // tight pixel budget, must fail rather than be trusted.
    Limits lim;
    lim.max_pixels = 100 * 100;
    std::string png = make_png(300, 300, false);
    std::string truncated = png.substr(0, png.size() / 2);
    REQUIRE_THROWS_AS(Transcoder(lim).transcode(truncated), ImageError);
}

//------------------------------------------------------------------------------

TEST_CASE("gemini/gpt presets relax the qwen constraints", "[image]") {
    // An extreme strip that the Qwen target pads to clamp the aspect ratio...
    std::string strip = make_png(4000, 10, false);
    REQUIRE(Transcoder(QwenLimits).transcode(strip).changed);

    // ...passes straight through under the looser presets (no aspect limit, no
    // per-side minimum), since it is a small, accepted-format image.
    for (const Limits& lim : {GeminiLimits, GptLimits}) {
        Transcoded t = Transcoder(lim).transcode(strip);
        REQUIRE_FALSE(t.changed);
        REQUIRE(t.format == Format::Png);
    }

    // Both looser presets allow a larger payload than Qwen.
    REQUIRE(GeminiLimits.max_bytes > QwenLimits.max_bytes);
    REQUIRE(GptLimits.max_bytes    > QwenLimits.max_bytes);
}

//------------------------------------------------------------------------------

TEST_CASE("to_data_url emits a well-formed data URL", "[image]") {
    std::string jpg = make_jpeg(100, 100, 85);
    Transcoder tc;

    std::string url = tc.to_data_url(jpg);
    const std::string prefix = "data:image/jpeg;base64,";
    REQUIRE(url.compare(0, prefix.size(), prefix) == 0);

    std::string decoded = b64_decode(url.substr(prefix.size()));
    REQUIRE(decoded == tc.transcode(jpg).bytes);
}
