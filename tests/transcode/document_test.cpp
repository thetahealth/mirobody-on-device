#include "transcode/document.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using mirobody::document::Document;
using mirobody::document::DocumentError;
using mirobody::document::Format;
using mirobody::document::Part;
using mirobody::document::Transcoder;

namespace {

// Process CSV input and return the single text part's Markdown.
std::string csv_to_md(const std::string& csv) {
    Transcoder tc;
    Document doc = tc.process(csv);
    REQUIRE(doc.format == Format::Csv);
    REQUIRE(doc.parts.size() == 1);
    REQUIRE(doc.parts[0].kind == Part::Kind::Text);
    return doc.parts[0].text;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

//------------------------------------------------------------------------------
// Format sniffing
//------------------------------------------------------------------------------

TEST_CASE("detect_format recognizes container magic bytes", "[document]") {
    REQUIRE(Transcoder::detect_format("%PDF-1.7\n...") == Format::Pdf);

    const std::string xlsx = std::string("PK\x03\x04", 4) + "rest-of-zip";
    REQUIRE(Transcoder::detect_format(xlsx) == Format::Xlsx);

    const std::string xls = std::string("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8) + "ole2";
    REQUIRE(Transcoder::detect_format(xls) == Format::Xls);

    REQUIRE(Transcoder::detect_format("a,b,c\n1,2,3\n") == Format::Csv);
    REQUIRE(Transcoder::detect_format("") == Format::Unknown);
}

TEST_CASE("mime_type maps each format", "[document]") {
    using mirobody::document::mime_type;
    REQUIRE(std::string(mime_type(Format::Pdf)) == "application/pdf");
    REQUIRE(contains(mime_type(Format::Xlsx), "spreadsheetml"));
    REQUIRE(std::string(mime_type(Format::Csv)) == "text/csv");
    REQUIRE(std::string(mime_type(Format::Unknown)) == "");
}

//------------------------------------------------------------------------------
// CSV -> Markdown
//------------------------------------------------------------------------------

TEST_CASE("CSV renders a Markdown table with a header separator", "[document][csv]") {
    std::string md = csv_to_md("name,age\nAda,36\nBob,40\n");
    REQUIRE(contains(md, "| name | age |"));
    REQUIRE(contains(md, "| --- | --- |"));
    REQUIRE(contains(md, "| Ada | 36 |"));
    REQUIRE(contains(md, "| Bob | 40 |"));
}

TEST_CASE("CSV honors quoted fields with commas, quotes and newlines", "[document][csv]") {
    // Field 1: a quoted comma; field 2: an escaped quote; field 3: an embedded newline.
    std::string md = csv_to_md("a,b,c\n\"x,y\",\"he said \"\"hi\"\"\",\"line1\nline2\"\n");
    REQUIRE(contains(md, "| x,y |"));            // comma preserved inside the cell
    REQUIRE(contains(md, "he said \"hi\""));     // doubled quote collapsed to one
    REQUIRE(contains(md, "line1<br>line2"));     // newline neutralized for the table
}

TEST_CASE("CSV escapes pipes so cells do not break columns", "[document][csv]") {
    std::string md = csv_to_md("h\na|b\n");
    REQUIRE(contains(md, "a\\|b"));
}

TEST_CASE("CSV pads ragged rows to the widest row", "[document][csv]") {
    // Header has 3 columns; the data row has 1 -> two trailing empty cells.
    std::string md = csv_to_md("a,b,c\nonly\n");
    REQUIRE(contains(md, "| only |  |  |"));
}

TEST_CASE("CSV strips a leading UTF-8 BOM from the first cell", "[document][csv]") {
    std::string md = csv_to_md(std::string("\xEF\xBB\xBF") + "name,age\nAda,36\n");
    REQUIRE(contains(md, "| name | age |"));
    REQUIRE_FALSE(contains(md, "\xEF\xBB\xBF"));
}

TEST_CASE("empty CSV yields a placeholder, not a crash", "[document][csv]") {
    Transcoder tc;
    Document doc = tc.process(std::string("\n"));
    REQUIRE(doc.parts.size() == 1);
    REQUIRE(contains(doc.parts[0].text, "empty"));
}

//------------------------------------------------------------------------------
// to_markdown
//------------------------------------------------------------------------------

TEST_CASE("to_markdown concatenates text parts", "[document]") {
    Transcoder tc;
    Document doc = tc.process("a,b\n1,2\n");
    std::string md = Transcoder::to_markdown(doc);
    REQUIRE(contains(md, "| a | b |"));
    REQUIRE(contains(md, "| 1 | 2 |"));
}

//------------------------------------------------------------------------------
// Gated formats throw a clear error when their handler was compiled out
//------------------------------------------------------------------------------

#ifndef MIROBODY_ENABLE_PDF
TEST_CASE("PDF without the PDF build throws a descriptive error", "[document][pdf]") {
    Transcoder tc;
    REQUIRE_THROWS_AS(tc.process("%PDF-1.4\n%fake"), DocumentError);
}
#endif

#ifndef MIROBODY_ENABLE_XLS
TEST_CASE("legacy .xls without the xls build throws a descriptive error", "[document][xls]") {
    Transcoder tc;
    const std::string xls = std::string("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8) + "ole2";
    REQUIRE_THROWS_AS(tc.process(xls), DocumentError);
}
#endif
