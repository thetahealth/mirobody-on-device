#include "fhir/units/normalize.hpp"

#include "fhir/units/families.hpp"
#include "fhir/units/tokens.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <unordered_map>

namespace mirobody { namespace fhir { namespace units {

namespace {

// ─── UTF-8 codepoint helpers ─────────────────────────────────────────
//
// The pipeline works on Unicode codepoints (as the Python original does on
// `str`), so we decode UTF-8 to a uint32_t sequence, transform, and re-encode.
// Invalid bytes are passed through as-is (treated as Latin-1) — clinical inputs
// are well-formed UTF-8 in practice and we never want to throw on junk.

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

std::string encode_utf8(const std::vector<uint32_t>& cps, size_t begin, size_t end) {
    std::string out;
    for (size_t i = begin; i < end && i < cps.size(); ++i) encode_utf8(cps[i], out);
    return out;
}

// NFKC-lite: 1:1 codepoint folding for the compatibility characters that occur
// in unit strings. Everything else passes through unchanged.
uint32_t nfkc_lite(uint32_t cp) {
    switch (cp) {
        // superscript digits -> ASCII digits
        case 0x2070: return '0';
        case 0x00B9: return '1';
        case 0x00B2: return '2';
        case 0x00B3: return '3';
        case 0x2074: return '4';
        case 0x2075: return '5';
        case 0x2076: return '6';
        case 0x2077: return '7';
        case 0x2078: return '8';
        case 0x2079: return '9';
        case 0x3000: return ' ';  // ideographic space
        case 0x00A0: return ' ';  // no-break space (Russian number/unit separator)
        case 0x202F: return ' ';  // narrow no-break space
        case 0x2007: return ' ';  // figure space
        case 0x2212: return '-';  // MINUS SIGN (Excel / PDF exports)
        case 0x207B: return '-';  // SUPERSCRIPT MINUS ("10⁻⁹")
        case 0x2010: return '-';  // HYPHEN
        case 0x2011: return '-';  // NON-BREAKING HYPHEN
        case 0x2013: return '-';  // EN DASH
        default: break;
    }
    // full-width ASCII variants U+FF01..U+FF5E -> U+0021..U+007E
    if (cp >= 0xFF01 && cp <= 0xFF5E) return cp - 0xFEE0;
    return cp;
}

bool is_ascii_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// Symbol fold applied per codepoint AFTER NFKC-lite + strip. Returns true and
// fills `rep` when the codepoint maps to a replacement (possibly empty, e.g.
// internal whitespace is dropped); returns false to keep the codepoint as-is.
bool symbol_fold(uint32_t cp, std::string& rep) {
    switch (cp) {
        case 0x00B5: rep = "u"; return true;    // MICRO SIGN
        case 0x03BC: rep = "u"; return true;    // GREEK SMALL LETTER MU
        case 0x00D7: rep = "x"; return true;    // MULTIPLICATION SIGN
        case 0x00B7: rep = "."; return true;    // MIDDLE DOT
        case 0x22C5: rep = "."; return true;    // DOT OPERATOR
        case 0x00B0: rep = "deg"; return true;  // DEGREE SIGN
        // Single-codepoint compatibility forms that real devices emit. NFKC
        // expands these to multiple characters, so they cannot live in the 1:1
        // nfkc_lite table. ℃ / ℉ come from CJK IMEs and lab analyzers; the
        // U+33xx "squared" units come from JIS-legacy Japanese exports.
        case 0x2103: rep = "degC"; return true;  // ℃
        case 0x2109: rep = "degF"; return true;  // ℉
        case 0x338E: rep = "mg";  return true;   // ㎎
        case 0x338F: rep = "kg";  return true;   // ㎏
        case 0x3383: rep = "mA";  return true;   // ㎃
        case 0x3396: rep = "mL";  return true;   // ㎖
        case 0x3397: rep = "dL";  return true;   // ㎗
        case 0x3398: rep = "kL";  return true;   // ㎘ -- kilolitre, never bare "L"
        case 0x339C: rep = "mm";  return true;   // ㎜
        case 0x339D: rep = "cm";  return true;   // ㎝
        case 0x339E: rep = "km";  return true;   // ㎞
        case 0x33A1: rep = "m2";  return true;   // ㎡
        case ' ':    rep = "";  return true;    // collapse internal whitespace
        case '\t':   rep = "";  return true;
        case '\n':   rep = "";  return true;    // hard-wrapped value+unit
        case '\r':   rep = "";  return true;
        case '\f':   rep = "";  return true;
        case '\v':   rep = "";  return true;
        default: return false;
    }
}

// Unicode-normalize and symbol-fold without case changes. Port of _clean().
std::string clean(const std::string& s) {
    std::vector<uint32_t> cps = decode_utf8(s);
    std::string nfkc;
    for (size_t i = 0; i < cps.size(); ++i) encode_utf8(nfkc_lite(cps[i]), nfkc);

    // strip()
    size_t b = 0, e = nfkc.size();
    while (b < e && is_ascii_ws(nfkc[b])) ++b;
    while (e > b && is_ascii_ws(nfkc[e - 1])) --e;
    if (b >= e) return std::string();
    std::string trimmed = nfkc.substr(b, e - b);

    std::vector<uint32_t> cps2 = decode_utf8(trimmed);
    std::string out;
    std::string rep;
    for (size_t i = 0; i < cps2.size(); ++i) {
        if (symbol_fold(cps2[i], rep)) out += rep;
        else encode_utf8(cps2[i], out);
    }
    return out;
}

// NFKC-lite + symbol fold but PRESERVE whitespace as a single space (token
// boundary for arbitrary-text scanning). Used by scan_value_units and by
// parse_value_unit's span fallbacks, which need the word boundary clean() drops.
std::string clean_keep_spaces(const std::string& s) {
    std::vector<uint32_t> cps = decode_utf8(s);
    std::string out;
    std::string rep;
    for (size_t i = 0; i < cps.size(); ++i) {
        uint32_t cp = nfkc_lite(cps[i]);
        if (cp == ' ' || cp == '\t') { out.push_back(' '); continue; }
        if (symbol_fold(cp, rep)) out += rep;
        else encode_utf8(cp, out);
    }
    return out;
}

// ─── inverted lookup tables ──────────────────────────────────────────
//
// invert {canonical: [tokens]} to {cleaned_token: canonical}, last-writer-wins
// on collisions (matching Python's `out[cleaned] = canon`). Insertion order is
// preserved in `order` for the morpheme length-sort tie-break.
struct InvertedTable {
    std::unordered_map<std::string, std::string> map;
    std::vector<std::string> order;  // cleaned keys, first-seen order
};

void invert_into(InvertedTable& t, const TokenTable& src) {
    for (size_t i = 0; i < src.size(); ++i) {
        const std::string& canon = src[i].first;
        const std::vector<std::string>& toks = src[i].second;
        for (size_t j = 0; j < toks.size(); ++j) {
            std::string c = clean(toks[j]);
            if (c.empty()) continue;
            std::unordered_map<std::string, std::string>::iterator it = t.map.find(c);
            if (it == t.map.end()) { t.map[c] = canon; t.order.push_back(c); }
            else { it->second = canon; }  // last wins, position kept
        }
    }
}

// setdefault: insert only if absent (keeps an explicit alias / earlier entry).
void set_default(InvertedTable& t, const std::string& key, const std::string& val) {
    if (key.empty()) return;
    if (t.map.find(key) == t.map.end()) { t.map[key] = val; t.order.push_back(key); }
}

const std::unordered_map<std::string, std::string>& alias_table() {
    static const std::unordered_map<std::string, std::string> m = [] {
        InvertedTable t;
        invert_into(t, aliases());
        // Every canonical UCUM unit maps to itself (explicit aliases win).
        const std::vector<std::pair<std::string, std::string> >& fam = ucum_family_ordered();
        for (size_t i = 0; i < fam.size(); ++i) set_default(t, clean(fam[i].first), fam[i].first);
        return t.map;
    }();
    return m;
}

struct MorphemeTable {
    std::unordered_map<std::string, std::string> map;
    std::vector<std::string> keys_by_len;  // sorted: most codepoints first, stable
};

size_t codepoint_count(const std::string& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) i += 1;
        else if ((c >> 5) == 0x6) i += 2;
        else if ((c >> 4) == 0xE) i += 3;
        else if ((c >> 3) == 0x1E) i += 4;
        else i += 1;
        ++n;
    }
    return n;
}

const MorphemeTable& morpheme_table() {
    static const MorphemeTable t = [] {
        InvertedTable inv;
        invert_into(inv, morphemes());
        // Auto-inject atomic UCUM canonicals (no '/' or '.') so the scanner
        // recognizes mg / mmol / L inside longer inputs. Compound canonicals
        // stay out (a "/L" here would let the greedy scan cross a boundary).
        const std::vector<std::pair<std::string, std::string> >& fam = ucum_family_ordered();
        for (size_t i = 0; i < fam.size(); ++i) {
            const std::string& canon = fam[i].first;
            if (canon.find('/') != std::string::npos || canon.find('.') != std::string::npos) continue;
            set_default(inv, clean(canon), canon);
        }
        set_default(inv, "/", "/");  // universal "per"

        MorphemeTable out;
        out.map = inv.map;
        // Stable sort the insertion-ordered keys by codepoint length desc.
        out.keys_by_len = inv.order;
        std::stable_sort(out.keys_by_len.begin(), out.keys_by_len.end(),
                         [](const std::string& a, const std::string& b) {
                             return codepoint_count(a) > codepoint_count(b);
                         });
        return out;
    }();
    return t;
}

// ─── annotation strip ────────────────────────────────────────────────
//
// Port of _strip_annotations (regex "/?\s*\{[^}]*\}"). Whitespace is already
// gone after clean(), so this removes each "{...}" plus a single immediately
// preceding "/". Leaves canonical annotation denominators intact only via the
// caller's exact-match alias hit before this runs.
std::string strip_annotations(const std::string& s) {
    std::string out;
    size_t i = 0, n = s.size();
    while (i < n) {
        if (s[i] == '{') {
            size_t close = s.find('}', i);
            if (close == std::string::npos) { out += s.substr(i); break; }
            if (!out.empty() && out[out.size() - 1] == '/') out.erase(out.size() - 1);
            i = close + 1;
        } else {
            out.push_back(s[i]);
            ++i;
        }
    }
    return out;
}

// ─── tokenize-compose ────────────────────────────────────────────────
//
// Depth-first with backtracking. The previous scan committed to the longest
// morpheme at each position and refused if that led to a dead end, which lost
// real spellings: "мед/л" matched "ме" -> [IU] and then could not finish, while
// the correct split "м"+"ед"+"/"+"л" -> mU/L was never tried. Keys are still
// visited longest-first, so where the greedy answer works it is still found
// first; only dead ends now cost a retry.
bool compose_dfs(const std::string& text, size_t pos, std::string& composed,
                 const MorphemeTable& t, int& budget) {
    if (--budget < 0) return false;
    if (pos == text.size()) return ucum_family_map().count(composed) != 0;
    for (size_t k = 0; k < t.keys_by_len.size(); ++k) {
        const std::string& tok = t.keys_by_len[k];
        if (tok.size() > text.size() - pos) continue;
        if (text.compare(pos, tok.size(), tok) != 0) continue;
        const std::string& frag = t.map.find(tok)->second;
        size_t keep = composed.size();
        // A "per" morpheme followed by a canonical that already carries its own
        // leading solidus is ONE solidus: "/" + "/[HPF]" is "/[HPF]", not
        // "//[HPF]" (which matched nothing, so "/高倍视野" failed while
        // "高倍视野" worked).
        if (!composed.empty() && composed[composed.size() - 1] == '/' &&
            !frag.empty() && frag[0] == '/')
            composed += frag.substr(1);
        else
            composed += frag;
        if (compose_dfs(text, pos + tok.size(), composed, t, budget)) return true;
        composed.resize(keep);
    }
    return false;
}

std::string tokenize_compose(const std::string& text) {
    std::string composed;
    int budget = 50000;  // backstop; unit strings are short
    if (compose_dfs(text, 0, composed, morpheme_table(), budget)) return composed;
    return std::string();
}

// ─── resolve ─────────────────────────────────────────────────────────
std::string resolve_strict(const std::string& cleaned) {
    if (cleaned.empty()) return std::string();
    const std::unordered_map<std::string, std::string>& alias = alias_table();
    std::unordered_map<std::string, std::string>::const_iterator it = alias.find(cleaned);
    if (it != alias.end()) return it->second;
    std::string stripped = strip_annotations(cleaned);
    if (stripped != cleaned) {
        it = alias.find(stripped);
        if (it != alias.end()) return it->second;
    }
    return tokenize_compose(stripped);
}

// The window for the edge resolvers is the longest key any table can offer. It
// used to be a flat 8 codepoints, which made every longer spelling unreachable
// ("миллиграмм" is 10) and then let the descent settle for a 1-codepoint match.
int edge_resolve_cap() {
    static const int cap = [] {
        size_t m = 0;
        const MorphemeTable& mt = morpheme_table();
        if (!mt.keys_by_len.empty()) m = codepoint_count(mt.keys_by_len[0]);
        const std::unordered_map<std::string, std::string>& al = alias_table();
        for (std::unordered_map<std::string, std::string>::const_iterator it = al.begin();
             it != al.end(); ++it)
            m = std::max(m, codepoint_count(it->first));
        return static_cast<int>(m);
    }();
    return cap;
}

// Letters of the alphabetic scripts the tables cover. Used only as a word-boundary
// test, so the exact block edges matter less than that CJK is excluded.
bool is_alpha_cp(uint32_t cp) {
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) return true;
    if (cp >= 0x00C0 && cp <= 0x024F) return true;  // Latin-1 suppl + extended
    if (cp >= 0x0370 && cp <= 0x03FF) return true;  // Greek
    if (cp >= 0x0400 && cp <= 0x04FF) return true;  // Cyrillic
    if (cp >= 0xAC00 && cp <= 0xD7A3) return true;  // Hangul syllables
    return false;
}

// Resolve the leading whitespace-delimited span, longest span first. clean()
// deletes spaces, and with them the boundary that separates a unit from a
// trailing noun -- "5 миллиграмм железа" became one 16-codepoint word, which no
// table entry matches. Words are re-joined WITHOUT spaces because that is the
// form the alias keys are cleaned into ("мм рт.ст." -> "ммрт.ст.").
std::string resolve_leading_span(const std::string& spaced) {
    std::vector<std::string> words;
    size_t i = 0;
    while (i < spaced.size()) {
        while (i < spaced.size() && spaced[i] == ' ') ++i;
        size_t b = i;
        while (i < spaced.size() && spaced[i] != ' ') ++i;
        if (i > b) words.push_back(spaced.substr(b, i - b));
    }
    for (size_t k = words.size(); k > 0; --k) {
        std::string joined;
        for (size_t j = 0; j < k; ++j) joined += words[j];
        std::string hit = resolve_strict(joined);
        if (!hit.empty()) return hit;
    }
    return std::string();
}

// Longest-prefix resolve over codepoints (handles compact CJK like "70克葡萄糖").
std::string resolve_prefix(const std::string& text) {
    if (text.empty()) return std::string();
    std::vector<uint32_t> cps = decode_utf8(text);
    int n = static_cast<int>(cps.size());
    int start = std::min(n, edge_resolve_cap());
    for (int end = start; end > 0; --end) {
        // A prefix may only end at a word boundary. Without this the descent to
        // ever shorter prefixes hands back a confident wrong unit: clean() drops
        // the space in "5 миллиграмм железа", and the 1-codepoint "м" -> m (Len)
        // matched. Ideographs are exempt -- they have no boundaries, and compact
        // forms like "70克葡萄糖" depend on the greedy match.
        if (end < n && is_alpha_cp(cps[end - 1]) && is_alpha_cp(cps[end])) continue;
        std::string hit = resolve_strict(encode_utf8(cps, 0, end));
        if (!hit.empty()) return hit;
    }
    return std::string();
}

// Longest-suffix resolve — mirror of resolve_prefix for the pre-value side.
std::string resolve_suffix(const std::string& text) {
    if (text.empty()) return std::string();
    std::vector<uint32_t> cps = decode_utf8(text);
    int n = static_cast<int>(cps.size());
    for (int s = std::max(0, n - edge_resolve_cap()); s < n; ++s) {
        if (s > 0 && is_alpha_cp(cps[s]) && is_alpha_cp(cps[s - 1])) continue;
        std::string hit = resolve_strict(encode_utf8(cps, s, n));
        if (!hit.empty()) return hit;
    }
    return std::string();
}

// ─── value scanning ──────────────────────────────────────────────────
//
// Comparator characters: the ASCII set plus the multibyte ≤ (U+2264),
// ≥ (U+2265), ≈ (U+2248) (3 bytes each in UTF-8). Mirrors the regex class
// [-<>=+~≤≥≈].
const char* kLe = "\xE2\x89\xA4";  // ≤
const char* kGe = "\xE2\x89\xA5";  // ≥
const char* kAp = "\xE2\x89\x88";  // ≈

const char* kLe2 = "\xE2\x89\xA6";  // ≦ (JIS)
const char* kGe2 = "\xE2\x89\xA7";  // ≧ (JIS)

bool is_ascii_cmp(char c) {
    return c == '-' || c == '<' || c == '>' || c == '=' || c == '+' || c == '~';
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Split a raw comparator run into a canonical comparator plus a sign. The last
// leading '+'/'-' belongs to the NUMBER ("<-5" is "less than minus five"); what
// precedes it must form exactly one relational operator or the input is refused
// rather than half-parsed -- match_value_prefix accepts any run of comparator
// characters, so "<>5" and "-+-+5" reach here and must not yield a value with a
// comparator that no FHIR consumer can serialize.
bool split_comparator(const std::string& raw, std::string& cmp, bool& negative) {
    cmp.clear();
    negative = false;
    std::string rel = raw;
    if (!rel.empty() && (rel[rel.size() - 1] == '-' || rel[rel.size() - 1] == '+')) {
        negative = rel[rel.size() - 1] == '-';
        rel.erase(rel.size() - 1);
    }
    if (rel.empty())                                   { cmp = "";   return true; }
    if (rel == "<")                                    { cmp = "<";  return true; }
    if (rel == ">")                                    { cmp = ">";  return true; }
    if (rel == "<=" || rel == "=<" || rel == kLe || rel == kLe2) { cmp = "<="; return true; }
    if (rel == ">=" || rel == "=>" || rel == kGe || rel == kGe2) { cmp = ">="; return true; }
    if (rel == "~" || rel == kAp)                      { cmp = "~";  return true; }
    if (rel == "=")                                    { cmp = "";   return true; }
    return false;  // "<>", "-+-+", "==" ... not a comparator
}

// Canonicalize a raw numeral to a dot-decimal string. With two or more group
// separators the LAST one is the decimal point and the earlier ones must delimit
// 3-digit groups ("1,234.5" and "1.234,5" are both 1234.5). A lone separator is
// the decimal point: this module treats "5,6" as the European decimal form (see
// normalize.hpp), so a US-style "1,234" reads as 1.234 by that same convention.
bool canonical_numeral(const std::string& raw, std::string& out) {
    if (raw.empty() || !is_digit(raw[0]) || !is_digit(raw[raw.size() - 1])) return false;
    std::vector<size_t> seps;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '.' || raw[i] == ',') seps.push_back(i);
        else if (!is_digit(raw[i])) return false;
    }
    if (seps.empty()) { out = raw; return true; }
    size_t dec = seps[seps.size() - 1];
    if (seps.size() > 1) {
        // Grouped form: 1-3 digits, then every separator 4 apart (sep + 3 digits).
        if (seps[0] < 1 || seps[0] > 3) return false;
        for (size_t k = 0; k + 1 < seps.size(); ++k)
            if (seps[k + 1] - seps[k] != 4) return false;
    }
    out.clear();
    for (size_t i = 0; i < raw.size(); ++i) {
        if (i == dec) out.push_back('.');
        else if (raw[i] != '.' && raw[i] != ',') out.push_back(raw[i]);
    }
    return true;
}

struct PrefixMatch {
    bool matched = false;
    std::string comparator;
    std::string number;
    size_t start_off = 0;  // byte offset of the number's first digit
    size_t end = 0;        // byte offset just past the number
};

// Scan a numeral at `start`: a digit run plus any number of [.,]+digits groups,
// so "1,234.5" is taken whole instead of stopping at the group separator (which
// used to leave a ".5" tail that the scanners then re-read as a second value).
// Which separator is the decimal point is decided later by canonical_numeral.
bool scan_numeral(const std::string& s, size_t start, size_t& end) {
    size_t i = start, n = s.size();
    if (i >= n || !is_digit(s[i])) return false;
    while (i < n && is_digit(s[i])) ++i;
    while (i + 1 < n && (s[i] == '.' || s[i] == ',') && is_digit(s[i + 1])) {
        ++i;
        while (i < n && is_digit(s[i])) ++i;
    }
    end = i;
    return true;
}

// Port of _VALUE_PREFIX.match: ^([-<>=+~≤≥≈]*)([0-9]+(?:[.,][0-9]+)?)
PrefixMatch match_value_prefix(const std::string& s) {
    PrefixMatch r;
    size_t i = 0, n = s.size();
    while (i < n) {
        if (is_ascii_cmp(s[i])) { r.comparator.push_back(s[i]); ++i; }
        else if (i + 3 <= n &&
                 (s.compare(i, 3, kLe) == 0 || s.compare(i, 3, kGe) == 0 ||
                  s.compare(i, 3, kAp) == 0 || s.compare(i, 3, kLe2) == 0 ||
                  s.compare(i, 3, kGe2) == 0)) {
            r.comparator.append(s, i, 3);
            i += 3;
        } else break;
    }
    size_t ds = i, e = 0;
    if (!scan_numeral(s, ds, e)) return r;  // no integer part -> no match
    r.matched = true;
    r.number = s.substr(ds, e - ds);
    r.end = e;
    return r;
}

// Port of _VALUE_ANYWHERE.search: first [0-9]+(?:[.,][0-9]+)? run anywhere.
// Returns matched=false if none; start_off/end bracket the run.
PrefixMatch search_value_anywhere(const std::string& s) {
    PrefixMatch r;
    size_t n = s.size();
    for (size_t i = 0; i < n; ++i) {
        if (!is_digit(s[i])) continue;
        size_t j = 0;
        if (!scan_numeral(s, i, j)) continue;
        r.matched = true;
        r.number = s.substr(i, j - i);
        r.start_off = i;
        r.end = j;
        return r;
    }
    return r;
}

// Decimal parse that does NOT go through std::stod: stod honours LC_NUMERIC, and
// a Qt host calls setlocale(LC_ALL, "") at startup, so in a comma-decimal locale
// stod("5.6") would stop at the '.' and every fractional value in the module
// would silently fail to parse. Digits are accumulated as one integer mantissa
// and scaled once, which also keeps "37.6" bit-identical to the literal.
bool parse_double(const std::string& s, double& out) {
    std::string canon;
    std::string body = s;
    bool neg = false;
    if (!body.empty() && (body[0] == '-' || body[0] == '+')) {
        neg = body[0] == '-';
        body.erase(0, 1);
    }
    if (!canonical_numeral(body, canon)) return false;

    double mant = 0.0;
    size_t frac_digits = 0;
    bool seen_dot = false;
    for (size_t i = 0; i < canon.size(); ++i) {
        if (canon[i] == '.') { seen_dot = true; continue; }
        mant = mant * 10.0 + static_cast<double>(canon[i] - '0');
        if (seen_dot) ++frac_digits;
        if (mant > 1e300) return false;  // absurd digit run
    }
    double scale = 1.0;
    for (size_t i = 0; i < frac_digits; ++i) scale *= 10.0;
    out = (mant / scale) * (neg ? -1.0 : 1.0);
    return true;
}

}  // namespace

// ─── public API ──────────────────────────────────────────────────────

std::string normalize_unit(const std::string& text) {
    if (text.empty()) return std::string();
    std::string cleaned = clean(text);
    if (cleaned.empty()) return std::string();
    std::string hit = resolve_strict(cleaned);
    if (!hit.empty()) return hit;
    // _VALUE_PREFIX.sub("", cleaned): strip a leading comparator+value if present.
    PrefixMatch m = match_value_prefix(cleaned);
    if (m.matched && m.end > 0) {
        std::string no_value = cleaned.substr(m.end);
        if (!no_value.empty() && no_value != cleaned) return resolve_strict(no_value);
    }
    return std::string();
}

ParsedQuantity parse_value_unit(const std::string& text) {
    ParsedQuantity empty;
    if (text.empty()) return empty;
    std::string cleaned = clean(text);
    if (cleaned.empty()) return empty;

    // Space-preserving copy, used only by the span fallbacks below.
    std::string spaced = clean_keep_spaces(text);
    {
        size_t b = 0, e = spaced.size();
        while (b < e && spaced[b] == ' ') ++b;
        while (e > b && spaced[e - 1] == ' ') --e;
        spaced = spaced.substr(b, e - b);
    }

    // Path A: whole input is a unit.
    std::string direct = resolve_strict(cleaned);
    if (!direct.empty()) return ParsedQuantity("", false, 0.0, direct);

    // Path B: value at start (optional comparator).
    PrefixMatch m = match_value_prefix(cleaned);
    if (m.matched) {
        std::string raw_cmp;
        bool negative = false;
        // "<-5" is "less than minus five": the sign belongs to the value, the
        // relation to the comparator. An unrecognizable run ("<>", "-+-+") has no
        // correct reading, so it is refused rather than reported with a bogus
        // comparator and a sign-stripped value.
        if (!split_comparator(m.comparator, raw_cmp, negative)) return empty;
        double value = 0.0;
        bool has_value = parse_double((negative ? "-" : "") + m.number, value);
        std::string rest = cleaned.substr(m.end);
        if (rest.empty()) return ParsedQuantity(raw_cmp, has_value, value, "");
        std::string unit = resolve_strict(rest);
        if (unit.empty()) unit = resolve_prefix(rest);  // compact CJK: "70克葡萄糖"
        if (unit.empty()) {                             // "5 миллиграмм железа"
            PrefixMatch sm = match_value_prefix(spaced);
            if (sm.matched) unit = resolve_leading_span(spaced.substr(sm.end));
        }
        if (!unit.empty()) return ParsedQuantity(raw_cmp, has_value, value, unit);
        // Rest exists but didn't resolve — fall through rather than emit a
        // half-parsed value-only result (e.g. "5.6/3.2").
    }

    // Path C: value anywhere (Chinese SVO: "每分钟90次").
    PrefixMatch nm = search_value_anywhere(cleaned);
    if (nm.matched) {
        std::string left = cleaned.substr(0, nm.start_off);
        std::string right = cleaned.substr(nm.end);
        // A '-' right before the digits is the value's sign, not part of the
        // preceding word ("BE -3 mmol/L" cleans to "BE-3mmol/L"). It is only a
        // sign when a digit does not precede it, so "5.6-7.8" stays a range and
        // is refused below instead of yielding a negative second value.
        bool negative = false;
        if (left.size() >= 1 && left[left.size() - 1] == '-' &&
            (left.size() == 1 || !is_digit(left[left.size() - 2]))) {
            negative = true;
            left.erase(left.size() - 1);
        }
        double value = 0.0;
        bool has_value = parse_double((negative ? "-" : "") + nm.number, value);
        std::string unit;
        if (!left.empty() || !right.empty()) unit = resolve_strict(left + right);
        if (unit.empty()) {
            unit = resolve_prefix(right);
            if (unit.empty()) unit = resolve_suffix(left);
        }
        if (unit.empty()) {  // "OGTT 口服 75 克 葡萄糖" — words after the value
            PrefixMatch sn = search_value_anywhere(spaced);
            if (sn.matched) unit = resolve_leading_span(spaced.substr(sn.end));
        }
        if (has_value && !unit.empty()) return ParsedQuantity("", true, value, unit);
    }

    return empty;
}

namespace {

bool is_dose_family(const std::string& ucum) {
    std::string fam = unit_family(ucum);
    return fam == "Mass" || fam == "Vol" || fam == "CCnt" || fam == "MCnt" || fam == "Arb";
}

bool is_unit_body_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '/' || c == '*' || c == '[' || c == ']' || c == '+' || c == '-';
}

bool is_unit_head_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

}  // namespace

std::vector<std::pair<double, std::string> > scan_value_units(const std::string& text) {
    std::vector<std::pair<double, std::string> > out;
    if (text.empty()) return out;
    std::string scan = clean_keep_spaces(text);
    size_t n = scan.size();

    std::vector<std::pair<double, std::string> > seen_keys;  // small; linear dedup
    auto record = [&](const std::string& raw_value, const std::string& ucum) {
        if (ucum.empty() || !is_dose_family(ucum)) return;
        double value;
        if (!parse_double(raw_value, value)) return;
        for (size_t i = 0; i < seen_keys.size(); ++i)
            if (seen_keys[i].first == value && seen_keys[i].second == ucum) return;
        seen_keys.push_back(std::make_pair(value, ucum));
        out.push_back(std::make_pair(value, ucum));
    };

    auto read_number = [&](size_t i, size_t& end) -> std::string {
        size_t j = i;
        if (!scan_numeral(scan, i, j)) { end = i; return std::string(); }
        end = j;
        return scan.substr(i, j - i);
    };

    // Pass 1 — Latin / micro / degree-shaped unit tokens (post-fold, so the head
    // is A-Za-z). number, optional spaces, then a <=12-char unit body, with a
    // trailing boundary that rejects mid-word matches like "75 grms".
    for (size_t i = 0; i < n; ) {
        if (!is_digit(scan[i])) { ++i; continue; }
        size_t num_end;
        std::string num = read_number(i, num_end);
        size_t p = num_end;
        while (p < n && scan[p] == ' ') ++p;
        if (p < n && is_unit_head_char(scan[p])) {
            size_t bs = p;
            size_t blen = 1;
            ++p;
            while (p < n && blen < 12 && is_unit_body_char(scan[p])) { ++p; ++blen; }
            // negative lookahead: next char must not be a Latin alnum.
            bool boundary_ok = (p >= n) ||
                               !((scan[p] >= 'A' && scan[p] <= 'Z') ||
                                 (scan[p] >= 'a' && scan[p] <= 'z') ||
                                 (scan[p] >= '0' && scan[p] <= '9'));
            if (boundary_ok) record(num, resolve_strict(clean(scan.substr(bs, p - bs))));
        }
        i = num_end;
    }

    // Pass 2 — CJK greedy unit parse via resolve_prefix, gated to non-ASCII
    // followers (Latin is Pass 1's territory).
    for (size_t i = 0; i < n; ) {
        if (!is_digit(scan[i])) { ++i; continue; }
        size_t num_end;
        std::string num = read_number(i, num_end);
        size_t p = num_end;
        while (p < n && (scan[p] == ' ' || scan[p] == '\t')) ++p;
        if (p < n && static_cast<unsigned char>(scan[p]) >= 0x80)
            record(num, resolve_prefix(scan.substr(p)));
        i = num_end;
    }

    return out;
}

}}}  // namespace mirobody::fhir::units
