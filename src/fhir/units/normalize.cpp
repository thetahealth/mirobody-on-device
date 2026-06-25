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
        case ' ':    rep = "";  return true;    // collapse internal whitespace
        case '\t':   rep = "";  return true;
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
std::string tokenize_compose(const std::string& text) {
    const MorphemeTable& t = morpheme_table();
    std::string composed;
    size_t pos = 0, n = text.size();
    while (pos < n) {
        bool matched = false;
        for (size_t k = 0; k < t.keys_by_len.size(); ++k) {
            const std::string& tok = t.keys_by_len[k];
            if (tok.size() <= n - pos && text.compare(pos, tok.size(), tok) == 0) {
                composed += t.map.find(tok)->second;
                pos += tok.size();
                matched = true;
                break;
            }
        }
        if (!matched) return std::string();  // unmatched char -> refuse
    }
    if (ucum_family_map().count(composed)) return composed;
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

const int kEdgeResolveCap = 8;

// Longest-prefix resolve over codepoints (handles compact CJK like "70克葡萄糖").
std::string resolve_prefix(const std::string& text) {
    if (text.empty()) return std::string();
    std::vector<uint32_t> cps = decode_utf8(text);
    int n = static_cast<int>(cps.size());
    int start = std::min(n, kEdgeResolveCap);
    for (int end = start; end > 0; --end) {
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
    for (int s = std::max(0, n - kEdgeResolveCap); s < n; ++s) {
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

bool is_ascii_cmp(char c) {
    return c == '-' || c == '<' || c == '>' || c == '=' || c == '+' || c == '~';
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

struct PrefixMatch {
    bool matched = false;
    std::string comparator;
    std::string number;
    size_t start_off = 0;  // byte offset of the number's first digit
    size_t end = 0;        // byte offset just past the number
};

// Port of _VALUE_PREFIX.match: ^([-<>=+~≤≥≈]*)([0-9]+(?:[.,][0-9]+)?)
PrefixMatch match_value_prefix(const std::string& s) {
    PrefixMatch r;
    size_t i = 0, n = s.size();
    while (i < n) {
        if (is_ascii_cmp(s[i])) { r.comparator.push_back(s[i]); ++i; }
        else if (i + 3 <= n &&
                 (s.compare(i, 3, kLe) == 0 || s.compare(i, 3, kGe) == 0 ||
                  s.compare(i, 3, kAp) == 0)) {
            r.comparator.append(s, i, 3);
            i += 3;
        } else break;
    }
    size_t ds = i;
    while (i < n && is_digit(s[i])) ++i;
    if (i == ds) return r;  // no integer part -> no match
    if (i < n && (s[i] == '.' || s[i] == ',') && i + 1 < n && is_digit(s[i + 1])) {
        ++i;
        while (i < n && is_digit(s[i])) ++i;
    }
    r.matched = true;
    r.number = s.substr(ds, i - ds);
    r.end = i;
    return r;
}

// Port of _VALUE_ANYWHERE.search: first [0-9]+(?:[.,][0-9]+)? run anywhere.
// Returns matched=false if none; start_off/end bracket the run.
PrefixMatch search_value_anywhere(const std::string& s) {
    PrefixMatch r;
    size_t n = s.size();
    for (size_t i = 0; i < n; ++i) {
        if (!is_digit(s[i])) continue;
        size_t ds = i, j = i;
        while (j < n && is_digit(s[j])) ++j;
        if (j < n && (s[j] == '.' || s[j] == ',') && j + 1 < n && is_digit(s[j + 1])) {
            ++j;
            while (j < n && is_digit(s[j])) ++j;
        }
        r.matched = true;
        r.number = s.substr(ds, j - ds);
        r.start_off = ds;
        r.end = j;
        return r;
    }
    return r;
}

bool parse_double(const std::string& s, double& out) {
    std::string t = s;
    for (size_t i = 0; i < t.size(); ++i) if (t[i] == ',') t[i] = '.';
    try {
        size_t idx = 0;
        double v = std::stod(t, &idx);
        if (idx != t.size()) return false;
        out = v;
        return true;
    } catch (const std::exception&) {
        return false;
    }
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

    // Path A: whole input is a unit.
    std::string direct = resolve_strict(cleaned);
    if (!direct.empty()) return ParsedQuantity("", false, 0.0, direct);

    // Path B: value at start (optional comparator).
    PrefixMatch m = match_value_prefix(cleaned);
    if (m.matched) {
        std::string raw_cmp = m.comparator;
        std::string sign;
        if (raw_cmp == "-") { sign = "-"; raw_cmp = ""; }
        else if (raw_cmp == "+") { raw_cmp = ""; }
        double value = 0.0;
        bool has_value = parse_double(sign + m.number, value);
        std::string rest = cleaned.substr(m.end);
        if (rest.empty()) return ParsedQuantity(raw_cmp, has_value, value, "");
        std::string unit = resolve_strict(rest);
        if (unit.empty()) unit = resolve_prefix(rest);  // compact CJK: "70克葡萄糖"
        if (!unit.empty()) return ParsedQuantity(raw_cmp, has_value, value, unit);
        // Rest exists but didn't resolve — fall through rather than emit a
        // half-parsed value-only result (e.g. "5.6/3.2").
    }

    // Path C: value anywhere (Chinese SVO: "每分钟90次").
    PrefixMatch nm = search_value_anywhere(cleaned);
    if (nm.matched) {
        double value = 0.0;
        bool has_value = parse_double(nm.number, value);
        std::string left = cleaned.substr(0, nm.start_off);
        std::string right = cleaned.substr(nm.end);
        std::string unit;
        if (!left.empty() || !right.empty()) unit = resolve_strict(left + right);
        if (unit.empty()) {
            unit = resolve_prefix(right);
            if (unit.empty()) unit = resolve_suffix(left);
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

// NFKC-lite + symbol fold but PRESERVE whitespace as a single space (token
// boundary for arbitrary-text scanning). Port of scan_value_units' inline clean.
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
        size_t ds = i, j = i;
        while (j < n && is_digit(scan[j])) ++j;
        if (j < n && (scan[j] == '.' || scan[j] == ',') && j + 1 < n && is_digit(scan[j + 1])) {
            ++j;
            while (j < n && is_digit(scan[j])) ++j;
        }
        end = j;
        return scan.substr(ds, j - ds);
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
