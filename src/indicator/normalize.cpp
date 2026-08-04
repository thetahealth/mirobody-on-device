#include "indicator/normalize.hpp"

#include <cstdint>
#include <vector>

namespace mirobody {
namespace indicator {

namespace {

// ─── UTF-8 codepoint helpers ─────────────────────────────────────────
// Same byte-level decode/encode as src/fhir/units/normalize.cpp. Invalid
// bytes pass through as Latin-1 rather than throwing — clinical inputs are
// well-formed UTF-8 in practice.

std::vector<uint32_t> decode_utf8(const std::string& s) {
    std::vector<uint32_t> cps;
    cps.reserve(s.size());
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp;
        size_t len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c >> 5) == 0x6 && i + 1 < n) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 0xE && i + 2 < n) { cp = c & 0x0F; len = 3; }
        else if ((c >> 3) == 0x1E && i + 3 < n) { cp = c & 0x07; len = 4; }
        else { cps.push_back(c); ++i; continue; }
        bool ok = true;
        for (size_t k = 1; k < len; ++k) {
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc >> 6) != 0x2) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { cps.push_back(c); ++i; continue; }
        cps.push_back(cp);
        i += len;
    }
    return cps;
}

void encode_utf8(uint32_t cp, std::string& out) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// NFKC-lite: 1:1 codepoint folds for compatibility characters that occur in
// clinical surface forms. Returns the codepoint that the input maps to.
uint32_t nfkc_lite(uint32_t cp) {
    switch (cp) {
        case 0x2070: return '0';  // superscript digits -> ASCII
        case 0x00B9: return '1';
        case 0x00B2: return '2';
        case 0x00B3: return '3';
        case 0x2074: return '4';
        case 0x2075: return '5';
        case 0x2076: return '6';
        case 0x2077: return '7';
        case 0x2078: return '8';
        case 0x2079: return '9';
        case 0x3000: return ' ';  // ideographic space -> ASCII space
        // Unicode hyphen/dash variants -> ASCII '-' (ChEBI IUPAC names use U+2010;
        // also en/em dash, figure dash, minus sign). Lets the tokenizer split on
        // them and matches ASCII-hyphen queries.
        case 0x2010: case 0x2011: case 0x2012: case 0x2013:
        case 0x2014: case 0x2015: case 0x2212: case 0x2043: return '-';
        default: break;
    }
    if (cp >= 0xFF01 && cp <= 0xFF5E) return cp - 0xFEE0;  // full-width ASCII
    return cp;
}

// Treat as whitespace for collapse/trim. ASCII whitespace only (ideographic
// space already folded to ' ' by nfkc_lite).
bool is_ws(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' ||
           cp == '\v';
}

}  // namespace

std::string normalize(const std::string& text) {
    std::vector<uint32_t> cps = decode_utf8(text);
    std::string out;
    out.reserve(text.size());
    bool pending_space = false;  // collapse runs; emit lazily so trailing ws drops
    bool wrote_any = false;
    for (size_t i = 0; i < cps.size(); ++i) {
        uint32_t cp = nfkc_lite(cps[i]);
        if (is_ws(cp)) {
            if (wrote_any) pending_space = true;  // ignore leading ws
            continue;
        }
        if (cp >= 'A' && cp <= 'Z') cp += 32;  // ASCII casefold
        if (pending_space) { out.push_back(' '); pending_space = false; }
        encode_utf8(cp, out);
        wrote_any = true;
    }
    return out;
}

}  // namespace mirobody::indicator
}  // namespace mirobody
