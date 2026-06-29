#include "transcode/image.hpp"

// libjpeg-turbo exposes the classic libjpeg API; <cstdio> must precede it
// because jpeglib.h references FILE in its (unused-by-us) stdio helpers.
#include <cstdio>
#include <csetjmp>
#include <jpeglib.h>

#include <png.h>

#include <webp/decode.h>

#include <tiffio.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace mirobody { namespace image {

namespace {

//------------------------------------------------------------------------------
// Decoded-image carrier
//------------------------------------------------------------------------------

// A decoded image as tightly packed 8-bit RGBA, row-major, top-left origin.
// Every decoder normalizes to this so the resize / encode stages are format-
// agnostic. `has_alpha` records whether the SOURCE carried real transparency,
// which is what decides PNG (keep alpha) vs JPEG (flatten) on re-encode.
struct RgbaImage {
    std::vector<std::uint8_t> px;
    int  w = 0;
    int  h = 0;
    bool has_alpha = false;

    std::uint8_t*       row(int y)       { return px.data() + static_cast<std::size_t>(y) * w * 4; }
    const std::uint8_t* row(int y) const { return px.data() + static_cast<std::size_t>(y) * w * 4; }
};

//------------------------------------------------------------------------------
// Base64 (standard alphabet, padded) — for to_data_url()
//------------------------------------------------------------------------------

std::string base64_encode(const std::string& in) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    const auto* d = reinterpret_cast<const std::uint8_t*>(in.data());
    for (std::size_t i = 0; i < in.size(); i += 3) {
        std::uint32_t n = static_cast<std::uint32_t>(d[i]) << 16;
        std::size_t take = 1;
        if (i + 1 < in.size()) { n |= static_cast<std::uint32_t>(d[i + 1]) << 8; take = 2; }
        if (i + 2 < in.size()) { n |= static_cast<std::uint32_t>(d[i + 2]);      take = 3; }
        out.push_back(tbl[(n >> 18) & 0x3F]);
        out.push_back(tbl[(n >> 12) & 0x3F]);
        out.push_back(take >= 2 ? tbl[(n >> 6) & 0x3F] : '=');
        out.push_back(take >= 3 ? tbl[n & 0x3F]        : '=');
    }
    return out;
}

//------------------------------------------------------------------------------
// JPEG (libjpeg-turbo)
//------------------------------------------------------------------------------

// libjpeg signals fatal errors by calling error_exit, whose default aborts the
// process. We override it to longjmp back to the caller so a bad image becomes
// an exception instead. (Kept free of C++ locals to avoid setjmp/longjmp
// teardown surprises.)
struct JpegErr {
    jpeg_error_mgr mgr;
    std::jmp_buf   jmp;
};

void jpeg_error_exit(j_common_ptr cinfo) {
    std::longjmp(reinterpret_cast<JpegErr*>(cinfo->err)->jmp, 1);
}
void jpeg_silence(j_common_ptr) {}

bool jpeg_probe(const std::uint8_t* data, std::size_t len, int& w, int& h) {
    jpeg_decompress_struct cinfo;
    JpegErr jerr;
    cinfo.err = jpeg_std_error(&jerr.mgr);
    jerr.mgr.error_exit = jpeg_error_exit;
    jerr.mgr.output_message = jpeg_silence;
    if (setjmp(jerr.jmp)) { jpeg_destroy_decompress(&cinfo); return false; }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, static_cast<unsigned long>(len));
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    w = static_cast<int>(cinfo.image_width);
    h = static_cast<int>(cinfo.image_height);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

RgbaImage jpeg_decode(const std::uint8_t* data, std::size_t len) {
    jpeg_decompress_struct cinfo;
    JpegErr jerr;
    cinfo.err = jpeg_std_error(&jerr.mgr);
    jerr.mgr.error_exit = jpeg_error_exit;
    jerr.mgr.output_message = jpeg_silence;
    if (setjmp(jerr.jmp)) { jpeg_destroy_decompress(&cinfo); throw ImageError("JPEG decode failed"); }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, static_cast<unsigned long>(len));
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    RgbaImage img;
    img.w = static_cast<int>(cinfo.output_width);
    img.h = static_cast<int>(cinfo.output_height);
    img.has_alpha = false;
    img.px.resize(static_cast<std::size_t>(img.w) * img.h * 4);

    std::vector<std::uint8_t> scan(static_cast<std::size_t>(img.w) * 3);
    while (cinfo.output_scanline < cinfo.output_height) {
        std::uint8_t* rowptr = scan.data();
        jpeg_read_scanlines(&cinfo, &rowptr, 1);
        std::uint8_t* dst = img.row(static_cast<int>(cinfo.output_scanline) - 1);
        for (int x = 0; x < img.w; ++x) {
            dst[x * 4 + 0] = scan[x * 3 + 0];
            dst[x * 4 + 1] = scan[x * 3 + 1];
            dst[x * 4 + 2] = scan[x * 3 + 2];
            dst[x * 4 + 3] = 255;
        }
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return img;
}

// Encode RGBA as baseline JPEG at `quality`, flattening any alpha onto white.
std::string jpeg_encode(const RgbaImage& img, int quality) {
    jpeg_compress_struct cinfo;
    JpegErr jerr;
    cinfo.err = jpeg_std_error(&jerr.mgr);
    jerr.mgr.error_exit = jpeg_error_exit;
    jerr.mgr.output_message = jpeg_silence;

    unsigned char* out = nullptr;
    unsigned long  out_size = 0;
    if (setjmp(jerr.jmp)) {
        jpeg_destroy_compress(&cinfo);
        if (out) free(out);
        throw ImageError("JPEG encode failed");
    }

    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &out, &out_size);
    cinfo.image_width = static_cast<JDIMENSION>(img.w);
    cinfo.image_height = static_cast<JDIMENSION>(img.h);
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, std::max(1, std::min(100, quality)), TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    std::vector<std::uint8_t> scan(static_cast<std::size_t>(img.w) * 3);
    while (cinfo.next_scanline < cinfo.image_height) {
        const std::uint8_t* src = img.row(static_cast<int>(cinfo.next_scanline));
        for (int x = 0; x < img.w; ++x) {
            std::uint8_t a = src[x * 4 + 3];
            // Composite over white: c' = c*a + 255*(255-a), /255.
            for (int c = 0; c < 3; ++c) {
                int v = src[x * 4 + c] * a + 255 * (255 - a);
                scan[x * 3 + c] = static_cast<std::uint8_t>((v + 127) / 255);
            }
        }
        std::uint8_t* rowptr = scan.data();
        jpeg_write_scanlines(&cinfo, &rowptr, 1);
    }
    jpeg_finish_compress(&cinfo);
    std::string result(reinterpret_cast<char*>(out), out_size);
    jpeg_destroy_compress(&cinfo);
    if (out) free(out);
    return result;
}

//------------------------------------------------------------------------------
// PNG (libpng simplified API)
//------------------------------------------------------------------------------

RgbaImage png_decode(const std::uint8_t* data, std::size_t len) {
    png_image image;
    std::memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;

    if (!png_image_begin_read_from_memory(&image, data, len)) {
        png_image_free(&image);
        throw ImageError("PNG decode failed (bad header)");
    }
    const bool had_alpha = (image.format & PNG_FORMAT_FLAG_ALPHA) != 0;
    image.format = PNG_FORMAT_RGBA;

    RgbaImage img;
    img.w = static_cast<int>(image.width);
    img.h = static_cast<int>(image.height);
    img.has_alpha = had_alpha;
    img.px.resize(PNG_IMAGE_SIZE(image));

    if (!png_image_finish_read(&image, nullptr, img.px.data(), 0, nullptr)) {
        png_image_free(&image);
        throw ImageError("PNG decode failed");
    }
    png_image_free(&image);
    return img;
}

std::string png_encode(const RgbaImage& img) {
    png_image image;
    std::memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    image.width = static_cast<png_uint_32>(img.w);
    image.height = static_cast<png_uint_32>(img.h);
    image.format = PNG_FORMAT_RGBA;

    png_alloc_size_t size = 0;
    if (!png_image_write_to_memory(&image, nullptr, &size, 0, img.px.data(), 0, nullptr)) {
        throw ImageError("PNG encode sizing failed");
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    if (!png_image_write_to_memory(&image, &out[0], &size, 0, img.px.data(), 0, nullptr)) {
        throw ImageError("PNG encode failed");
    }
    out.resize(static_cast<std::size_t>(size));
    return out;
}

//------------------------------------------------------------------------------
// WebP (libwebp)
//------------------------------------------------------------------------------

RgbaImage webp_decode(const std::uint8_t* data, std::size_t len) {
    WebPBitstreamFeatures feat;
    std::memset(&feat, 0, sizeof(feat));
    if (WebPGetFeatures(data, len, &feat) != VP8_STATUS_OK) {
        throw ImageError("WebP decode failed (bad header)");
    }
    int w = 0, h = 0;
    std::uint8_t* rgba = WebPDecodeRGBA(data, len, &w, &h);
    if (!rgba) throw ImageError("WebP decode failed");

    RgbaImage img;
    img.w = w;
    img.h = h;
    img.has_alpha = feat.has_alpha != 0;
    img.px.assign(rgba, rgba + static_cast<std::size_t>(w) * h * 4);
    WebPFree(rgba);
    return img;
}

//------------------------------------------------------------------------------
// TIFF (libtiff over an in-memory client)
//------------------------------------------------------------------------------

struct MemTiff {
    const std::uint8_t* data;
    toff_t              size;
    toff_t              pos;
};

tmsize_t tiff_read(thandle_t h, void* buf, tmsize_t n) {
    auto* m = static_cast<MemTiff*>(h);
    if (m->pos >= m->size) return 0;
    toff_t avail = m->size - m->pos;
    tmsize_t take = static_cast<tmsize_t>(std::min<toff_t>(static_cast<toff_t>(n), avail));
    std::memcpy(buf, m->data + m->pos, static_cast<std::size_t>(take));
    m->pos += static_cast<toff_t>(take);
    return take;
}
tmsize_t tiff_write(thandle_t, void*, tmsize_t) { return 0; }
toff_t tiff_seek(thandle_t h, toff_t off, int whence) {
    auto* m = static_cast<MemTiff*>(h);
    if (whence == SEEK_SET)      m->pos = off;
    else if (whence == SEEK_CUR) m->pos += off;
    else if (whence == SEEK_END) m->pos = m->size + off;
    return m->pos;
}
int    tiff_close(thandle_t) { return 0; }
toff_t tiff_size(thandle_t h) { return static_cast<MemTiff*>(h)->size; }
int    tiff_map(thandle_t, void**, toff_t*) { return 0; }
void   tiff_unmap(thandle_t, void*, toff_t) {}

RgbaImage tiff_decode(const std::uint8_t* data, std::size_t len) {
    // Silence libtiff's default stderr chatter on quirky-but-decodable files.
    TIFFSetWarningHandler(nullptr);

    MemTiff mem{data, static_cast<toff_t>(len), 0};
    TIFF* tif = TIFFClientOpen("mem", "r", &mem,
                               tiff_read, tiff_write, tiff_seek, tiff_close,
                               tiff_size, tiff_map, tiff_unmap);
    if (!tif) throw ImageError("TIFF decode failed (open)");

    std::uint32_t w = 0, h = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    if (w == 0 || h == 0) { TIFFClose(tif); throw ImageError("TIFF decode failed (dimensions)"); }

    std::uint16_t extra = 0, *sampleinfo = nullptr;
    bool has_alpha = TIFFGetField(tif, TIFFTAG_EXTRASAMPLES, &extra, &sampleinfo) && extra > 0;

    RgbaImage img;
    img.w = static_cast<int>(w);
    img.h = static_cast<int>(h);
    img.has_alpha = has_alpha;
    img.px.resize(static_cast<std::size_t>(w) * h * 4);

    // TIFFReadRGBAImageOriented yields packed ABGR uint32 with a top-left
    // origin; the TIFFGet* macros pull the channels out portably.
    std::vector<std::uint32_t> raster(static_cast<std::size_t>(w) * h);
    if (!TIFFReadRGBAImageOriented(tif, w, h, raster.data(), ORIENTATION_TOPLEFT, 0)) {
        TIFFClose(tif);
        throw ImageError("TIFF decode failed (raster)");
    }
    for (std::size_t i = 0; i < raster.size(); ++i) {
        std::uint32_t p = raster[i];
        img.px[i * 4 + 0] = static_cast<std::uint8_t>(TIFFGetR(p));
        img.px[i * 4 + 1] = static_cast<std::uint8_t>(TIFFGetG(p));
        img.px[i * 4 + 2] = static_cast<std::uint8_t>(TIFFGetB(p));
        img.px[i * 4 + 3] = static_cast<std::uint8_t>(TIFFGetA(p));
    }
    TIFFClose(tif);
    return img;
}

//------------------------------------------------------------------------------
// Resampling
//------------------------------------------------------------------------------

// Area-average downscale: each destination pixel is the mean of the source
// pixels its footprint covers. Good quality for the common case (shrinking a
// large photo) without the aliasing a single bilinear tap would introduce.
RgbaImage downscale_area(const RgbaImage& src, int dw, int dh) {
    RgbaImage dst;
    dst.w = dw; dst.h = dh; dst.has_alpha = src.has_alpha;
    dst.px.resize(static_cast<std::size_t>(dw) * dh * 4);
    for (int ty = 0; ty < dh; ++ty) {
        long long sy0 = static_cast<long long>(ty) * src.h / dh;
        long long sy1 = static_cast<long long>(ty + 1) * src.h / dh;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int tx = 0; tx < dw; ++tx) {
            long long sx0 = static_cast<long long>(tx) * src.w / dw;
            long long sx1 = static_cast<long long>(tx + 1) * src.w / dw;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            std::uint32_t acc[4] = {0, 0, 0, 0};
            std::uint32_t cnt = 0;
            for (long long sy = sy0; sy < sy1; ++sy) {
                const std::uint8_t* r = src.row(static_cast<int>(sy));
                for (long long sx = sx0; sx < sx1; ++sx) {
                    const std::uint8_t* p = r + sx * 4;
                    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3];
                    ++cnt;
                }
            }
            std::uint8_t* o = dst.row(ty) + tx * 4;
            for (int c = 0; c < 4; ++c) o[c] = static_cast<std::uint8_t>(acc[c] / cnt);
        }
    }
    return dst;
}

// Bilinear resample — used when any dimension grows (we rarely upscale, but the
// min-side floor can require it for a degenerate tiny input).
RgbaImage resample_bilinear(const RgbaImage& src, int dw, int dh) {
    RgbaImage dst;
    dst.w = dw; dst.h = dh; dst.has_alpha = src.has_alpha;
    dst.px.resize(static_cast<std::size_t>(dw) * dh * 4);
    const double fx = src.w > 1 ? static_cast<double>(src.w - 1) / std::max(1, dw - 1) : 0.0;
    const double fy = src.h > 1 ? static_cast<double>(src.h - 1) / std::max(1, dh - 1) : 0.0;
    for (int ty = 0; ty < dh; ++ty) {
        double syf = ty * fy;
        int sy = static_cast<int>(syf);
        double wy = syf - sy;
        int sy1 = std::min(sy + 1, src.h - 1);
        for (int tx = 0; tx < dw; ++tx) {
            double sxf = tx * fx;
            int sx = static_cast<int>(sxf);
            double wx = sxf - sx;
            int sx1 = std::min(sx + 1, src.w - 1);
            const std::uint8_t* p00 = src.row(sy) + sx * 4;
            const std::uint8_t* p01 = src.row(sy) + sx1 * 4;
            const std::uint8_t* p10 = src.row(sy1) + sx * 4;
            const std::uint8_t* p11 = src.row(sy1) + sx1 * 4;
            std::uint8_t* o = dst.row(ty) + tx * 4;
            for (int c = 0; c < 4; ++c) {
                double top = p00[c] * (1 - wx) + p01[c] * wx;
                double bot = p10[c] * (1 - wx) + p11[c] * wx;
                o[c] = static_cast<std::uint8_t>(top * (1 - wy) + bot * wy + 0.5);
            }
        }
    }
    return dst;
}

RgbaImage resize_to(const RgbaImage& src, int dw, int dh) {
    if (dw == src.w && dh == src.h) return src;
    if (dw <= src.w && dh <= src.h) return downscale_area(src, dw, dh);
    return resample_bilinear(src, dw, dh);
}

// Center `src` onto a `dw`x`dh` opaque-white canvas (alpha 255). Used to bring
// an over-long aspect ratio within bounds by padding the short side rather than
// cropping content away.
RgbaImage pad_centered(const RgbaImage& src, int dw, int dh) {
    RgbaImage dst;
    dst.w = dw; dst.h = dh; dst.has_alpha = src.has_alpha;
    dst.px.assign(static_cast<std::size_t>(dw) * dh * 4, 255);
    int ox = (dw - src.w) / 2;
    int oy = (dh - src.h) / 2;
    for (int y = 0; y < src.h; ++y) {
        std::memcpy(dst.row(y + oy) + ox * 4, src.row(y), static_cast<std::size_t>(src.w) * 4);
    }
    return dst;
}

}  // namespace

//------------------------------------------------------------------------------
// Public interface
//------------------------------------------------------------------------------

// Provider presets. QwenLimits is just the in-class defaults; the looser two are
// built field-by-field (a small lambda, so each stays a single const instance
// without relying on C++14 aggregate-init of a struct with member initializers).
const Limits QwenLimits = Limits();

const Limits GeminiLimits = [] {
    Limits l;
    l.max_bytes  = 13u * 1024 * 1024;  // ~13 MB raw -> ~17 MB base64, under Gemini's 20 MB request cap
    l.min_side   = 1;                  // Gemini imposes no per-side minimum
    l.max_pixels = 3072LL * 3072;      // soft cap to bound tiles / token cost
    l.max_aspect = 1.0e9;              // effectively unlimited (Gemini has no aspect limit)
    return l;
}();

const Limits GptLimits = [] {
    Limits l;
    l.max_bytes  = 20u * 1024 * 1024;  // OpenAI's single-image cap
    l.min_side   = 1;                  // no per-side minimum
    l.max_pixels = 2048LL * 2048;      // GPT scales everything into a 2048x2048 box
    l.max_aspect = 1.0e9;              // no aspect limit
    return l;
}();

const char* mime_type(Format f) {
    switch (f) {
        case Format::Jpeg: return "image/jpeg";
        case Format::Png:  return "image/png";
        case Format::WebP: return "image/webp";
        case Format::Tiff: return "image/tiff";
        default:           return "";
    }
}

Format Transcoder::detect_format(const std::string& in) {
    const auto* d = reinterpret_cast<const std::uint8_t*>(in.data());
    std::size_t n = in.size();
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return Format::Jpeg;
    if (n >= 8 && d[0] == 0x89 && d[1] == 0x50 && d[2] == 0x4E && d[3] == 0x47 &&
        d[4] == 0x0D && d[5] == 0x0A && d[6] == 0x1A && d[7] == 0x0A) return Format::Png;
    if (n >= 12 && std::memcmp(d, "RIFF", 4) == 0 && std::memcmp(d + 8, "WEBP", 4) == 0)
        return Format::WebP;
    if (n >= 4 && ((d[0] == 'I' && d[1] == 'I' && d[2] == 0x2A && d[3] == 0x00) ||
                   (d[0] == 'M' && d[1] == 'M' && d[2] == 0x00 && d[3] == 0x2A)))
        return Format::Tiff;
    return Format::Unknown;
}

Transcoder::Transcoder(Limits limits) : limits_(limits) {}

Transcoded Transcoder::transcode(const std::string& input) const {
    const auto* data = reinterpret_cast<const std::uint8_t*>(input.data());
    const std::size_t len = input.size();

    const Format fmt = detect_format(input);
    if (fmt == Format::Unknown) {
        throw ImageError("unsupported or unrecognized image format "
                         "(expected JPEG, PNG, WebP, or TIFF)");
    }

    const double max_aspect = limits_.max_aspect;
    const int    min_side   = limits_.min_side;
    const long long max_px  = limits_.max_pixels;

    auto dims_ok = [&](int w, int h) {
        if (w < min_side || h < min_side) return false;
        if (static_cast<long long>(w) * h > max_px) return false;
        double aspect = static_cast<double>(std::max(w, h)) / std::min(w, h);
        return aspect <= max_aspect;
    };

    // Fast path: an already-compliant JPEG/PNG/WebP is returned verbatim. Probe
    // dimensions from the header only — no full decode.
    if ((fmt == Format::Jpeg || fmt == Format::Png || fmt == Format::WebP) &&
        len <= limits_.max_bytes) {
        int w = 0, h = 0;
        bool probed = false;
        if (fmt == Format::Jpeg) {
            probed = jpeg_probe(data, len, w, h);
        } else if (fmt == Format::Png) {
            // IHDR width/height are the two big-endian u32s at byte offset 16.
            if (len >= 24) {
                w = (data[16] << 24) | (data[17] << 16) | (data[18] << 8) | data[19];
                h = (data[20] << 24) | (data[21] << 16) | (data[22] << 8) | data[23];
                probed = true;
            }
        } else {  // WebP
            probed = WebPGetInfo(data, len, &w, &h) != 0;
        }
        if (probed && dims_ok(w, h)) {
            Transcoded out;
            out.bytes = std::string(input);
            out.content_type = mime_type(fmt);
            out.width = w;
            out.height = h;
            out.format = fmt;
            out.changed = false;
            return out;
        }
    }

    // Re-encode path: decode to RGBA.
    RgbaImage img;
    switch (fmt) {
        case Format::Jpeg: img = jpeg_decode(data, len); break;
        case Format::Png:  img = png_decode(data, len);  break;
        case Format::WebP: img = webp_decode(data, len); break;
        case Format::Tiff: img = tiff_decode(data, len); break;
        default:           throw ImageError("unreachable");
    }
    if (img.w <= 0 || img.h <= 0) throw ImageError("decoded image has no pixels");

    // Target size: shrink to the pixel budget, then enforce the min-side floor
    // (scaling up uniformly if the image is degenerate-tiny).
    int tw = img.w, th = img.h;
    if (static_cast<long long>(tw) * th > max_px) {
        double s = std::sqrt(static_cast<double>(max_px) /
                             (static_cast<double>(tw) * th));
        tw = std::max(1, static_cast<int>(tw * s));
        th = std::max(1, static_cast<int>(th * s));
    }
    if (tw < min_side || th < min_side) {
        double s = std::max(static_cast<double>(min_side) / tw,
                            static_cast<double>(min_side) / th);
        tw = std::max(min_side, static_cast<int>(std::ceil(tw * s)));
        th = std::max(min_side, static_cast<int>(std::ceil(th * s)));
    }

    RgbaImage cur = resize_to(img, tw, th);

    // Aspect clamp: pad the short side (centered, on white) so long/short does
    // not exceed max_aspect.
    {
        double aspect = static_cast<double>(std::max(cur.w, cur.h)) / std::min(cur.w, cur.h);
        if (aspect > max_aspect) {
            if (cur.w > cur.h) {
                int need_h = static_cast<int>(std::ceil(cur.w / max_aspect));
                cur = pad_centered(cur, cur.w, std::max(cur.h, need_h));
            } else {
                int need_w = static_cast<int>(std::ceil(cur.h / max_aspect));
                cur = pad_centered(cur, std::max(cur.w, need_w), cur.h);
            }
        }
    }

    // Encode within the byte budget. PNG (lossless) when the source had real
    // transparency, otherwise JPEG. If the result overflows max_bytes, step the
    // JPEG quality down, then shrink dimensions, until it fits or we hit the
    // min-side floor.
    Transcoded out;
    out.changed = true;
    out.width = cur.w;
    out.height = cur.h;

    if (img.has_alpha) {
        std::string enc = png_encode(cur);
        while (enc.size() > limits_.max_bytes &&
               cur.w > min_side * 2 && cur.h > min_side * 2) {
            cur = resize_to(cur, std::max(min_side, cur.w * 4 / 5),
                                 std::max(min_side, cur.h * 4 / 5));
            enc = png_encode(cur);
        }
        if (enc.size() <= limits_.max_bytes) {
            out.bytes = std::move(enc);
            out.content_type = "image/png";
            out.format = Format::Png;
            out.width = cur.w;
            out.height = cur.h;
            return out;
        }
        // PNG still too large (rare; high-entropy alpha image). Fall through to
        // a lossy JPEG, which composites the alpha onto white.
    }

    int quality = limits_.jpeg_quality;
    std::string enc = jpeg_encode(cur, quality);
    int guard = 0;
    while (enc.size() > limits_.max_bytes && guard++ < 64) {
        if (quality > 30) {
            quality -= 10;
        } else if (cur.w > min_side * 2 && cur.h > min_side * 2) {
            cur = resize_to(cur, std::max(min_side, cur.w * 4 / 5),
                                 std::max(min_side, cur.h * 4 / 5));
        } else {
            break;  // floor reached; emit the smallest we can
        }
        enc = jpeg_encode(cur, quality);
    }
    out.bytes = std::move(enc);
    out.content_type = "image/jpeg";
    out.format = Format::Jpeg;
    out.width = cur.w;
    out.height = cur.h;
    return out;
}

std::string Transcoder::to_data_url(const std::string& input) const {
    Transcoded t = transcode(input);
    std::string url = "data:";
    url += t.content_type;
    url += ";base64,";
    url += base64_encode(t.bytes);
    return url;
}

}}
