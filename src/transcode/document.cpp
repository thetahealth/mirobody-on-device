#include "transcode/document.hpp"

#ifdef MIROBODY_ENABLE_XLSX
#include <xlnt/xlnt.hpp>
#endif

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

// PDF (PDFium) and OCR (Tesseract) are optional; see the gating note in
// document.hpp. The headers are pulled in only for the builds that link them.
#ifdef MIROBODY_ENABLE_PDF
#include <fpdfview.h>
#include <fpdf_text.h>
#include <png.h>       // raster bitmap -> PNG before handing to image::Transcoder
#include <cmath>
#include <mutex>
#ifdef MIROBODY_ENABLE_OCR
#include <tesseract/baseapi.h>
#endif
#endif

#ifdef MIROBODY_ENABLE_XLS
#include <xls.h>
#endif

namespace mirobody { namespace document {

namespace {

//------------------------------------------------------------------------------
// Markdown table rendering (shared by CSV / xlsx / xls)
//------------------------------------------------------------------------------

// Escape a cell for inclusion in a Markdown table: pipes break columns and
// newlines break rows, so neutralize both; everything else passes through.
std::string md_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '|') { out += "\\|"; }
        else if (c == '\r') { /* drop; handled with \n */ }
        else if (c == '\n') { out += "<br>"; }
        else { out.push_back(c); }
    }
    return out;
}

// Render a row-major grid as a GitHub-flavored Markdown table. The first row is
// the header; a grid with no rows yields "". Ragged rows are padded to the
// widest row so every line has the same column count.
std::string grid_to_markdown(const std::vector<std::vector<std::string> >& rows) {
    if (rows.empty()) return std::string();
    std::size_t cols = 0;
    for (std::size_t r = 0; r < rows.size(); ++r) cols = std::max(cols, rows[r].size());
    if (cols == 0) return std::string();

    std::ostringstream out;
    auto emit_row = [&](const std::vector<std::string>& row) {
        out << '|';
        for (std::size_t c = 0; c < cols; ++c) {
            out << ' ' << (c < row.size() ? md_escape(row[c]) : std::string()) << " |";
        }
        out << '\n';
    };

    emit_row(rows[0]);
    out << '|';
    for (std::size_t c = 0; c < cols; ++c) out << " --- |";
    out << '\n';
    for (std::size_t r = 1; r < rows.size(); ++r) emit_row(rows[r]);
    return out.str();
}

//------------------------------------------------------------------------------
// CSV (RFC 4180)
//------------------------------------------------------------------------------

// Parse RFC-4180 CSV into a grid: comma field separator, CRLF or LF record
// separator, double-quoted fields may contain commas/quotes/newlines, and a
// quote inside a quoted field is escaped by doubling ("").
std::vector<std::vector<std::string> > parse_csv(const std::string& in) {
    std::vector<std::vector<std::string> > rows;
    std::vector<std::string> row;
    std::string field;
    bool in_quotes = false;
    bool field_started = false;  // distinguishes a trailing-newline empty line from a real empty row
    const char* d = in.data();
    std::size_t n = in.size();
    // Strip a leading UTF-8 BOM (EF BB BF) -- Excel-exported CSVs commonly carry
    // one, and it would otherwise contaminate the first header cell.
    if (n >= 3 && static_cast<std::uint8_t>(d[0]) == 0xEF &&
                  static_cast<std::uint8_t>(d[1]) == 0xBB &&
                  static_cast<std::uint8_t>(d[2]) == 0xBF) {
        d += 3;
        n -= 3;
    }

    auto end_field = [&]() { row.push_back(field); field.clear(); field_started = false; };
    auto end_row   = [&]() { end_field(); rows.push_back(row); row.clear(); };

    for (std::size_t i = 0; i < n; ++i) {
        char c = d[i];
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < n && d[i + 1] == '"') { field.push_back('"'); ++i; }
                else { in_quotes = false; }
            } else {
                field.push_back(c);
            }
            continue;
        }
        if (c == '"') { in_quotes = true; field_started = true; }
        else if (c == ',') { field_started = true; end_field(); }
        else if (c == '\n') { end_row(); }
        else if (c == '\r') { /* swallow; the \n (or EOF) ends the row */ }
        else { field.push_back(c); field_started = true; }
    }
    // Flush a final record that wasn't newline-terminated.
    if (field_started || !field.empty() || !row.empty()) end_row();
    return rows;
}

//------------------------------------------------------------------------------
// xlsx (xlnt) -- one Markdown table per worksheet. Built when xlnt is available
// (always on Windows via vcpkg; auto-detected elsewhere).
//------------------------------------------------------------------------------

#ifdef MIROBODY_ENABLE_XLSX

void process_xlsx(const std::string& input, Document& doc, std::size_t max_pages) {
    std::vector<std::uint8_t> bytes(
        reinterpret_cast<const std::uint8_t*>(input.data()),
        reinterpret_cast<const std::uint8_t*>(input.data()) + input.size());

    xlnt::workbook wb;
    try {
        wb.load(bytes);
    } catch (const std::exception& e) {
        throw DocumentError(std::string("xlsx decode failed: ") + e.what());
    }

    int sheet_index = 0;
    std::size_t emitted = 0;
    for (auto ws : wb) {
        ++sheet_index;
        if (max_pages != 0 && emitted >= max_pages) {
            Part note;
            note.kind = Part::Kind::Text;
            note.text = "_(remaining sheets omitted: max_pages reached)_";
            note.page = sheet_index;
            doc.parts.push_back(note);
            break;
        }

        const xlnt::row_t hr = ws.highest_row();
        const xlnt::column_t::index_t hc = ws.highest_column().index;

        std::vector<std::vector<std::string> > grid;
        bool any_value = false;
        for (xlnt::row_t r = 1; r <= hr; ++r) {
            std::vector<std::string> line;
            line.reserve(hc);
            for (xlnt::column_t::index_t c = 1; c <= hc; ++c) {
                xlnt::cell_reference ref(static_cast<xlnt::column_t>(c), r);
                std::string v = ws.has_cell(ref) ? ws.cell(ref).to_string() : std::string();
                if (!v.empty()) any_value = true;
                line.push_back(v);
            }
            grid.push_back(line);
        }
        if (!any_value) grid.clear();  // an all-blank sheet renders as the placeholder below

        Part p;
        p.kind = Part::Kind::Text;
        p.page = sheet_index;
        std::ostringstream os;
        os << "## " << ws.title() << "\n\n";
        std::string table = grid_to_markdown(grid);
        os << (table.empty() ? "_(empty sheet)_\n" : table);
        p.text = os.str();
        doc.parts.push_back(p);
        ++emitted;
    }
}

#endif  // MIROBODY_ENABLE_XLSX

//------------------------------------------------------------------------------
// PDF (PDFium) -- gated
//------------------------------------------------------------------------------

#ifdef MIROBODY_ENABLE_PDF

// PDFium's library init/teardown is process-global and not thread-safe, and a
// loaded document may not be used concurrently. A single mutex serializes all
// PDFium work; init happens once under std::call_once and is never torn down
// (the cost is a one-time leak of PDFium's globals, which is the conventional
// trade-off for a long-lived service).
std::mutex& pdfium_mutex() { static std::mutex m; return m; }

void pdfium_init_once() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        FPDF_LIBRARY_CONFIG cfg;
        std::memset(&cfg, 0, sizeof(cfg));
        cfg.version = 2;
        FPDF_InitLibraryWithConfig(&cfg);
    });
}

// Encode a tightly packed RGBA buffer to PNG (libpng simplified API), so the
// rasterized page can be fed through the existing image::Transcoder.
std::string rgba_to_png(const std::uint8_t* rgba, int w, int h) {
    png_image image;
    std::memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    image.width  = static_cast<png_uint_32>(w);
    image.height = static_cast<png_uint_32>(h);
    image.format = PNG_FORMAT_RGBA;

    png_alloc_size_t size = 0;
    if (!png_image_write_to_memory(&image, nullptr, &size, 0, rgba, 0, nullptr))
        throw DocumentError("PDF page PNG sizing failed");
    std::string out(static_cast<std::size_t>(size), '\0');
    if (!png_image_write_to_memory(&image, &out[0], &size, 0, rgba, 0, nullptr))
        throw DocumentError("PDF page PNG encode failed");
    out.resize(static_cast<std::size_t>(size));
    return out;
}

void process_pdf(const std::string& input, const Options& opts, Document& doc) {
    std::lock_guard<std::mutex> lock(pdfium_mutex());
    pdfium_init_once();

    FPDF_DOCUMENT pdf = FPDF_LoadMemDocument(
        input.data(), static_cast<int>(input.size()), nullptr);
    if (!pdf) throw DocumentError("PDF decode failed (load)");

    const int pages = FPDF_GetPageCount(pdf);
    const image::Transcoder img_tc(opts.image_limits);
    const double scale = opts.raster_dpi / 72.0;  // PDF user space is 72 dpi

    for (int i = 0; i < pages; ++i) {
        if (opts.max_pages != 0 && static_cast<std::size_t>(i) >= opts.max_pages) {
            Part note;
            note.text = "_(remaining pages omitted: max_pages reached)_";
            note.page = i + 1;
            doc.parts.push_back(note);
            break;
        }

        FPDF_PAGE page = FPDF_LoadPage(pdf, i);
        if (!page) continue;

        // Text layer first.
        std::string text;
        FPDF_TEXTPAGE tp = FPDFText_LoadPage(page);
        if (tp) {
            int chars = FPDFText_CountChars(tp);
            if (chars > 0) {
                // GetText returns UTF-16LE including a trailing NUL; ask for
                // chars+1 units, then transcode the BMP subset to UTF-8.
                std::vector<unsigned short> buf(static_cast<std::size_t>(chars) + 1, 0);
                int got = FPDFText_GetText(tp, 0, chars, buf.data());
                for (int k = 0; k < got && buf[k]; ++k) {
                    unsigned int cp = buf[k];
                    if (cp < 0x80) {
                        text.push_back(static_cast<char>(cp));
                    } else if (cp < 0x800) {
                        text.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    } else {
                        text.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                }
            }
            FPDFText_ClosePage(tp);
        }

        const bool has_text = static_cast<int>(text.size()) >= opts.pdf_text_threshold;
        if (has_text) {
            Part p;
            p.kind = Part::Kind::Text;
            p.page = i + 1;
            p.text = text;
            doc.parts.push_back(p);
            FPDF_ClosePage(page);
            continue;
        }

        // Scanned page: rasterize, then transcode to a vision-compliant image.
        int w = static_cast<int>(std::ceil(FPDF_GetPageWidth(page) * scale));
        int h = static_cast<int>(std::ceil(FPDF_GetPageHeight(page) * scale));
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        FPDF_BITMAP bmp = FPDFBitmap_Create(w, h, 1 /*alpha*/);
        if (!bmp) { FPDF_ClosePage(page); continue; }
        FPDFBitmap_FillRect(bmp, 0, 0, w, h, 0xFFFFFFFF);
        FPDF_RenderPageBitmap(bmp, page, 0, 0, w, h, 0, FPDF_ANNOT);

        // PDFium buffer is BGRA; repack to RGBA for PNG.
        const std::uint8_t* src = static_cast<const std::uint8_t*>(FPDFBitmap_GetBuffer(bmp));
        const int stride = FPDFBitmap_GetStride(bmp);
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4);
        for (int y = 0; y < h; ++y) {
            const std::uint8_t* s = src + static_cast<std::size_t>(y) * stride;
            std::uint8_t* dpx = rgba.data() + static_cast<std::size_t>(y) * w * 4;
            for (int x = 0; x < w; ++x) {
                dpx[x * 4 + 0] = s[x * 4 + 2];
                dpx[x * 4 + 1] = s[x * 4 + 1];
                dpx[x * 4 + 2] = s[x * 4 + 0];
                dpx[x * 4 + 3] = s[x * 4 + 3];
            }
        }

        try {
            std::string png = rgba_to_png(rgba.data(), w, h);
            Part p;
            p.kind = Part::Kind::Image;
            p.page = i + 1;
            p.image = img_tc.transcode(png);
            doc.parts.push_back(p);
        } catch (const std::exception&) {
            // If the image step fails we still drop OCR below; just skip the image.
        }

#ifdef MIROBODY_ENABLE_OCR
        if (opts.ocr_enabled) {
            tesseract::TessBaseAPI api;
            const char* dp = opts.ocr_datapath.empty() ? nullptr : opts.ocr_datapath.c_str();
            if (api.Init(dp, opts.ocr_lang.c_str()) == 0) {
                api.SetImage(rgba.data(), w, h, 4, w * 4);
                char* utf8 = api.GetUTF8Text();
                if (utf8) {
                    std::string ocr(utf8);
                    delete[] utf8;
                    if (!ocr.empty()) {
                        Part p;
                        p.kind = Part::Kind::Text;
                        p.page = i + 1;
                        p.text = ocr;
                        doc.parts.push_back(p);
                    }
                }
                api.End();
            }
        }
#endif

        FPDFBitmap_Destroy(bmp);
        FPDF_ClosePage(page);
    }

    FPDF_CloseDocument(pdf);
}

#endif  // MIROBODY_ENABLE_PDF

//------------------------------------------------------------------------------
// Legacy .xls (libxls) -- gated
//------------------------------------------------------------------------------

#ifdef MIROBODY_ENABLE_XLS

void process_xls(const std::string& input, Document& doc, std::size_t max_pages) {
    xls_error_t err = LIBXLS_OK;
    xlsWorkBook* wb = xls_open_buffer(
        reinterpret_cast<const unsigned char*>(input.data()), input.size(), "UTF-8", &err);
    if (!wb) throw DocumentError(std::string("xls decode failed: ") + xls_getError(err));

    std::size_t emitted = 0;
    for (unsigned int s = 0; s < wb->sheets.count; ++s) {
        if (max_pages != 0 && emitted >= max_pages) break;
        xlsWorkSheet* ws = xls_getWorkSheet(wb, static_cast<int>(s));
        if (!ws || xls_parseWorkSheet(ws) != LIBXLS_OK) continue;

        std::vector<std::vector<std::string> > grid;
        if (ws->rows.lastrow > 0 || ws->rows.lastcol > 0) {
            for (WORD r = 0; r <= ws->rows.lastrow; ++r) {
                std::vector<std::string> line;
                for (WORD c = 0; c <= ws->rows.lastcol; ++c) {
                    xlsCell* cell = xls_cell(ws, r, c);
                    line.push_back(cell && cell->str ? std::string(reinterpret_cast<const char*>(cell->str))
                                                      : std::string());
                }
                grid.push_back(line);
            }
        }

        Part p;
        p.kind = Part::Kind::Text;
        p.page = static_cast<int>(s) + 1;
        std::ostringstream os;
        os << "## " << (wb->sheets.sheet[s].name ? reinterpret_cast<const char*>(wb->sheets.sheet[s].name) : "")
           << "\n\n";
        std::string table = grid_to_markdown(grid);
        os << (table.empty() ? "_(empty sheet)_\n" : table);
        p.text = os.str();
        doc.parts.push_back(p);
        ++emitted;
        xls_close_WS(ws);
    }
    xls_close_WB(wb);
}

#endif  // MIROBODY_ENABLE_XLS

}  // namespace

//------------------------------------------------------------------------------
// Public interface
//------------------------------------------------------------------------------

const char* mime_type(Format f) {
    switch (f) {
        case Format::Pdf:  return "application/pdf";
        case Format::Xlsx: return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
        case Format::Xls:  return "application/vnd.ms-excel";
        case Format::Csv:  return "text/csv";
        default:           return "";
    }
}

Format Transcoder::detect_format(const std::string& in) {
    const auto* d = reinterpret_cast<const std::uint8_t*>(in.data());
    const std::size_t n = in.size();
    if (n >= 5 && std::memcmp(d, "%PDF-", 5) == 0) return Format::Pdf;
    // .xlsx (and any OOXML) is a ZIP: "PK\x03\x04".
    if (n >= 4 && d[0] == 'P' && d[1] == 'K' && d[2] == 0x03 && d[3] == 0x04) return Format::Xlsx;
    // Legacy .xls is an OLE2 compound file: D0 CF 11 E0 A1 B1 1A E1.
    if (n >= 8 && d[0] == 0xD0 && d[1] == 0xCF && d[2] == 0x11 && d[3] == 0xE0 &&
        d[4] == 0xA1 && d[5] == 0xB1 && d[6] == 0x1A && d[7] == 0xE1) return Format::Xls;
    // No reliable magic for CSV; treat any other non-empty input as CSV.
    if (n > 0) return Format::Csv;
    return Format::Unknown;
}

Transcoder::Transcoder(Options opts) : opts_(opts) {}

Document Transcoder::process(const std::string& input) const {
    Document doc;
    doc.format = detect_format(input);

    switch (doc.format) {
        case Format::Csv: {
            std::vector<std::vector<std::string> > grid = parse_csv(input);
            // A grid with no non-empty cell (e.g. blank lines only) is treated
            // as empty rather than rendered as a table of empty cells.
            bool any_value = false;
            for (std::size_t r = 0; r < grid.size() && !any_value; ++r)
                for (std::size_t c = 0; c < grid[r].size(); ++c)
                    if (!grid[r][c].empty()) { any_value = true; break; }
            Part p;
            p.kind = Part::Kind::Text;
            p.page = 1;
            std::string table = any_value ? grid_to_markdown(grid) : std::string();
            p.text = table.empty() ? std::string("_(empty CSV)_\n") : table;
            doc.parts.push_back(p);
            break;
        }
        case Format::Xlsx:
#ifdef MIROBODY_ENABLE_XLSX
            process_xlsx(input, doc, opts_.max_pages);
#else
            throw DocumentError("xlsx support was not built (xlnt not found at configure time)");
#endif
            break;
        case Format::Xls:
#ifdef MIROBODY_ENABLE_XLS
            process_xls(input, doc, opts_.max_pages);
#else
            throw DocumentError("legacy .xls support was not built (MIROBODY_ENABLE_XLS off)");
#endif
            break;
        case Format::Pdf:
#ifdef MIROBODY_ENABLE_PDF
            process_pdf(input, opts_, doc);
#else
            throw DocumentError("PDF support was not built (MIROBODY_ENABLE_PDF off)");
#endif
            break;
        default:
            throw DocumentError("unsupported or empty document input");
    }
    return doc;
}

std::string Transcoder::to_markdown(const Document& doc) {
    std::ostringstream os;
    for (std::size_t i = 0; i < doc.parts.size(); ++i) {
        const Part& p = doc.parts[i];
        if (p.kind == Part::Kind::Image) {
            os << "![page " << p.page << " image](" << mirobody::image::mime_type(p.image.format)
               << ", " << p.image.width << "x" << p.image.height << ")\n";
        } else {
            os << p.text;
            if (!p.text.empty() && p.text[p.text.size() - 1] != '\n') os << '\n';
        }
        os << '\n';
    }
    return os.str();
}

}}
