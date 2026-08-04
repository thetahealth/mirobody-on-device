#include "indicator/word.hpp"

#include "indicator/normalize.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace mirobody {
namespace indicator {

std::vector<std::string> word_tokens(const std::string& text) {
    std::string s = normalize(text);
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char b = static_cast<unsigned char>(s[i]);
        // token byte: ASCII alnum, or any non-ASCII byte (CJK / Greek / …).
        bool tok = (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') || (b >= 0x80);
        if (tok) cur.push_back(static_cast<char>(b));
        else if (!cur.empty()) { out.push_back(cur); cur.clear(); }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool build_word_vocab(const std::string& in_tsv, const std::string& out_tsv,
                      int col, uint32_t min_count, std::string& err) {
    // Review filters (applied here, NOT in word_tokens): drop pure-number tokens
    // and stopwords. Single letters are KEPT (K = potassium, vitamin A/D, IgA).
    static const std::set<std::string> STOP = {
        "of", "in", "and", "or", "with", "by", "the", "for", "to", "an", "on",
        "at", "as", "is", "are", "from", "per", "no", "non", "without", "wo"};

    std::ifstream in(in_tsv.c_str(), std::ios::binary);
    if (!in) { err = "cannot read " + in_tsv; return false; }
    std::unordered_map<std::string, uint32_t> cnt;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        if (header) { header = false; continue; }
        // pull column `col` (1-based)
        int f = 1; size_t start = 0; std::string field; bool got = false;
        for (size_t i = 0; i <= line.size(); ++i) {
            if (i == line.size() || line[i] == '\t') {
                if (f == col) { field = line.substr(start, i - start); got = true; break; }
                ++f; start = i + 1;
            }
        }
        if (!got) continue;
        std::vector<std::string> toks = word_tokens(field);
        for (size_t k = 0; k < toks.size(); ++k) {
            const std::string& t = toks[k];
            bool num = true;
            for (size_t j = 0; j < t.size(); ++j)
                if (t[j] < '0' || t[j] > '9') { num = false; break; }
            if (num) continue;                              // pure number
            if (STOP.find(t) != STOP.end()) continue;       // stopword
            cnt[t]++;
        }
    }

    std::vector<std::pair<uint32_t, const std::string*> > v;
    v.reserve(cnt.size());
    for (std::unordered_map<std::string, uint32_t>::iterator it = cnt.begin();
         it != cnt.end(); ++it)
        v.push_back(std::make_pair(it->second, &it->first));
    std::sort(v.begin(), v.end(),
              [](const std::pair<uint32_t, const std::string*>& a,
                 const std::pair<uint32_t, const std::string*>& b) {
                  if (a.first != b.first) return a.first > b.first;
                  return *a.second < *b.second;
              });

    FILE* fo = std::fopen(out_tsv.c_str(), "wb");
    if (!fo) { err = "cannot write " + out_tsv; return false; }
    std::fputs("word\tcount\n", fo);
    size_t written = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i].first < min_count) break;  // sorted desc -> rest are below too
        std::fprintf(fo, "%s\t%u\n", v[i].second->c_str(), v[i].first);
        ++written;
    }
    std::fclose(fo);
    std::fprintf(stderr, "words: %zu tokens (>=%u) of %zu distinct from %s -> %s\n",
                 written, min_count, v.size(), in_tsv.c_str(), out_tsv.c_str());
    return true;
}

namespace {

// Stopwords shared with build_word_vocab (functional words carry no synonymy).
const std::set<std::string>& stopwords() {
    static const std::set<std::string> STOP = {
        "of", "in", "and", "or", "with", "by", "the", "for", "to", "an", "on",
        "at", "as", "is", "are", "from", "per", "no", "non", "without", "wo"};
    return STOP;
}

// Catalog-metadata tokens that ride along on drug surfaces but are not clinical
// terms. They look like words, so they have no lexical pattern to filter on — a
// curated list is the right instrument. Dropped like stopwords so they never
// become synonym-pair endpoints. This list is topped up as new catalog junk is
// observed; [bracketed] packaging is also stripped before tokenizing, but store
// brands also appear bare ("… well at walgreens") so they are listed too.
const std::set<std::string>& struct_tags() {
    static const std::set<std::string> T = {
        // RxNorm nomenclature suffixes / registry / source labels
        "inn", "usan", "ban", "jan", "jp", "jp17", "jp18", "rac", "iso", "mi",
        "vandf", "hsdb", "fcc", "hpus", "nf", "crs", "rs", "monograph",
        "standard", "deprecated", "hplc", "mart",
        // US OTC store brands (dual-use tokens whose role here is packaging)
        "cvs", "walgreens", "gnp", "leader", "sunmark", "goodsense", "equate",
        "equaline", "publix", "kirkland", "heb", "kroger", "topcare", "careone",
        "careall", "qualitest"};
    return T;
}

// Copy `s` dropping any [...] bracketed segment (RxNorm brand/packaging cruft).
std::string strip_brackets(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    int depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '[') ++depth;
        else if (c == ']') { if (depth > 0) --depth; }
        else if (depth == 0) out.push_back(c);
    }
    return out;
}

inline uint64_t pack_pair(uint32_t a, uint32_t b) {
    if (a > b) std::swap(a, b);
    return (static_cast<uint64_t>(a) << 32) | b;
}

// True if a,b are the same unit at different magnitudes ("1000ug"/"100ug",
// "100ml"/"ml") — i.e. they agree after stripping a leading digit run. Keeps
// ordinals (2nd/second), salt counts (2hcl/dihydrochloride) and fatty-acid
// notation (1n9/1w9), whose remainders differ.
bool magnitude_variant(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && a[i] >= '0' && a[i] <= '9') ++i;
    while (j < b.size() && b[j] >= '0' && b[j] <= '9') ++j;
    if (i == 0 && j == 0) return false;  // neither is quantity-prefixed
    return i < a.size() && a.compare(i, std::string::npos, b, j, std::string::npos) == 0;
}

// If sorted sets a,b (equal length) differ by exactly one element each, set
// x=a's unique, y=b's unique and return true; else false.
bool single_sub_diff(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b,
                     uint32_t& x, uint32_t& y) {
    size_t i = 0, j = 0, na = 0, nb = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] == b[j]) { ++i; ++j; }
        else if (a[i] < b[j]) { x = a[i]; ++i; if (++na > 1) return false; }
        else { y = b[j]; ++j; if (++nb > 1) return false; }
    }
    while (i < a.size()) { x = a[i]; ++i; if (++na > 1) return false; }
    while (j < b.size()) { y = b[j]; ++j; if (++nb > 1) return false; }
    return na == 1 && nb == 1;
}

}  // namespace

bool build_synonyms(const std::string& in_tsv, const std::string& out_tsv,
                    uint32_t min_support, size_t max_tokens, size_t max_group,
                    const std::string& lex_dir, std::string& err) {
    std::ifstream in(in_tsv.c_str(), std::ios::binary);
    if (!in) { err = "cannot read " + in_tsv; return false; }
    const std::set<std::string>& STOP = stopwords();
    const std::set<std::string>& TAGS = struct_tags();

    std::unordered_map<std::string, uint32_t> word_id;
    std::vector<std::string> words;
    std::unordered_map<std::string, uint32_t> concept_id;
    std::vector<uint32_t> surf_cid;               // concept id per kept surface
    std::vector<std::vector<uint32_t> > surf_set; // sorted unique word ids

    std::string line;
    bool header = true;
    std::vector<uint32_t> buf;
    while (std::getline(in, line)) {
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        if (header) { header = false; continue; }
        std::string::size_type p1 = line.find('\t');
        if (p1 == std::string::npos) continue;
        std::string::size_type p2 = line.find('\t', p1 + 1);
        if (p2 == std::string::npos) continue;
        std::string::size_type p3 = line.find('\t', p2 + 1);
        std::string surface = line.substr(0, p1);
        // concept key = "system\tcode"
        std::string ckey = line.substr(p1 + 1, (p3 == std::string::npos ? line.size() : p3) - (p1 + 1));

        std::vector<std::string> toks = word_tokens(strip_brackets(surface));
        bool dropped_num = false;
        buf.clear();
        for (size_t k = 0; k < toks.size(); ++k) {
            const std::string& t = toks[k];
            bool num = true;
            for (size_t j = 0; j < t.size(); ++j)
                if (t[j] < '0' || t[j] > '9') { num = false; break; }
            if (num) { dropped_num = true; continue; }  // pure number
            if (STOP.find(t) != STOP.end()) continue;
            if (TAGS.find(t) != TAGS.end()) continue;   // catalog-metadata tag
            std::unordered_map<std::string, uint32_t>::iterator it = word_id.find(t);
            uint32_t id;
            if (it != word_id.end()) id = it->second;
            else { id = static_cast<uint32_t>(words.size()); word_id.emplace(t, id); words.push_back(t); }
            buf.push_back(id);
        }
        std::sort(buf.begin(), buf.end());
        buf.erase(std::unique(buf.begin(), buf.end()), buf.end());
        size_t sz = buf.size();
        if (sz == 0 || sz > max_tokens) continue;
        if (sz <= 1 && dropped_num) continue;  // identifier surface ("refchem:100")

        std::unordered_map<std::string, uint32_t>::iterator ci = concept_id.find(ckey);
        uint32_t cid;
        if (ci != concept_id.end()) cid = ci->second;
        else { cid = static_cast<uint32_t>(concept_id.size()); concept_id.emplace(ckey, cid); }
        surf_cid.push_back(cid);
        surf_set.push_back(buf);
    }
    std::fprintf(stderr, "synonyms: %zu surfaces, %zu concepts, %zu distinct words\n",
                 surf_set.size(), concept_id.size(), words.size());

    // Group surfaces by concept (index sort), mine single-substitution pairs.
    std::vector<uint32_t> idx(surf_cid.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = static_cast<uint32_t>(i);
    std::sort(idx.begin(), idx.end(),
              [&](uint32_t a, uint32_t b) { return surf_cid[a] < surf_cid[b]; });

    std::unordered_map<uint64_t, uint32_t> support;
    std::vector<const std::vector<uint32_t>*> g;
    uint32_t x, y;
    for (size_t s = 0; s < idx.size();) {
        size_t e = s;
        uint32_t cid = surf_cid[idx[s]];
        while (e < idx.size() && surf_cid[idx[e]] == cid) ++e;
        g.clear();
        for (size_t k = s; k < e; ++k) g.push_back(&surf_set[idx[k]]);
        s = e;
        // distinct surfaces only
        std::sort(g.begin(), g.end(),
                  [](const std::vector<uint32_t>* a, const std::vector<uint32_t>* b) { return *a < *b; });
        g.erase(std::unique(g.begin(), g.end(),
                            [](const std::vector<uint32_t>* a, const std::vector<uint32_t>* b) { return *a == *b; }),
                g.end());
        if (g.size() < 2 || g.size() > max_group) continue;
        for (size_t i = 0; i < g.size(); ++i)
            for (size_t j = i + 1; j < g.size(); ++j) {
                if (g[i]->size() != g[j]->size()) continue;
                if (single_sub_diff(*g[i], *g[j], x, y)) support[pack_pair(x, y)]++;
            }
    }

    // Keep support >= min_support; then drop pairs whose words co-occur in a surface.
    std::unordered_set<uint64_t> candidate;
    std::unordered_set<uint32_t> involved;
    for (std::unordered_map<uint64_t, uint32_t>::iterator it = support.begin(); it != support.end(); ++it)
        if (it->second >= min_support) {
            candidate.insert(it->first);
            involved.insert(static_cast<uint32_t>(it->first >> 32));
            involved.insert(static_cast<uint32_t>(it->first & 0xFFFFFFFFu));
        }
    std::unordered_set<uint64_t> cooc;
    std::vector<uint32_t> present;
    for (size_t i = 0; i < surf_set.size(); ++i) {
        const std::vector<uint32_t>& sset = surf_set[i];
        present.clear();
        for (size_t k = 0; k < sset.size(); ++k)
            if (involved.find(sset[k]) != involved.end()) present.push_back(sset[k]);
        for (size_t a = 0; a < present.size(); ++a)
            for (size_t b = a + 1; b < present.size(); ++b) {
                uint64_t key = pack_pair(present[a], present[b]);
                if (candidate.find(key) != candidate.end()) cooc.insert(key);
            }
    }

    // Assemble corpus survivors as ordered string pairs with a provenance
    // bitmask (1 = corpus, 2 = lex).
    typedef std::map<std::pair<std::string, std::string>, std::pair<uint32_t, int> > PairMap;
    PairMap merged;
    for (std::unordered_set<uint64_t>::iterator it = candidate.begin(); it != candidate.end(); ++it) {
        if (cooc.find(*it) != cooc.end()) continue;
        const std::string& wa = words[static_cast<uint32_t>(*it >> 32)];
        const std::string& wb = words[static_cast<uint32_t>(*it & 0xFFFFFFFFu)];
        if (magnitude_variant(wa, wb)) continue;  // dose magnitudes, not synonyms
        std::pair<std::string, std::string> key =
            (wa < wb) ? std::make_pair(wa, wb) : std::make_pair(wb, wa);
        std::pair<uint32_t, int>& m = merged[key];
        m.first = support[*it];
        m.second |= 1;
    }
    size_t n_corpus = merged.size();

    // Merge authoritative single-token spelling variants from the SPECIALIST
    // Lexicon (LRSPL: EUI|spelling_variant|citation|). Curated, so bypass the
    // support and co-occurrence gates.
    size_t n_lex = 0;
    if (!lex_dir.empty()) {
        std::string lrspl = lex_dir + "/LRSPL";
        std::ifstream lx(lrspl.c_str(), std::ios::binary);
        if (!lx) {
            std::fprintf(stderr, "synonyms: (optional) LRSPL not read: %s\n", lrspl.c_str());
        } else {
            std::string ll;
            while (std::getline(lx, ll)) {
                if (!ll.empty() && ll[ll.size() - 1] == '\r') ll.erase(ll.size() - 1);
                std::string::size_type b1 = ll.find('|');
                if (b1 == std::string::npos) continue;
                std::string::size_type b2 = ll.find('|', b1 + 1);
                if (b2 == std::string::npos) continue;
                std::string::size_type b3 = ll.find('|', b2 + 1);
                std::string spv = ll.substr(b1 + 1, b2 - b1 - 1);
                std::string cit = ll.substr(b2 + 1, (b3 == std::string::npos ? ll.size() : b3) - (b2 + 1));
                std::vector<std::string> ta = word_tokens(spv), tb = word_tokens(cit);
                if (ta.size() != 1 || tb.size() != 1 || ta[0] == tb[0]) continue;
                std::pair<std::string, std::string> key =
                    (ta[0] < tb[0]) ? std::make_pair(ta[0], tb[0]) : std::make_pair(tb[0], ta[0]);
                merged[key].second |= 2;
                ++n_lex;
            }
        }
    }

    std::vector<PairMap::const_iterator> rows;
    rows.reserve(merged.size());
    for (PairMap::const_iterator it = merged.begin(); it != merged.end(); ++it) rows.push_back(it);
    std::sort(rows.begin(), rows.end(),
              [](PairMap::const_iterator a, PairMap::const_iterator b) {
                  if (a->second.first != b->second.first) return a->second.first > b->second.first;
                  return a->first < b->first;
              });

    FILE* fo = std::fopen(out_tsv.c_str(), "wb");
    if (!fo) { err = "cannot write " + out_tsv; return false; }
    std::fputs("word_a\tword_b\tsupport\tsource\n", fo);
    for (size_t i = 0; i < rows.size(); ++i) {
        int src = rows[i]->second.second;
        const char* s = (src == 3) ? "corpus+lex" : (src == 2) ? "lex" : "corpus";
        std::fprintf(fo, "%s\t%s\t%u\t%s\n", rows[i]->first.first.c_str(),
                     rows[i]->first.second.c_str(), rows[i]->second.first, s);
    }
    std::fclose(fo);
    std::fprintf(stderr,
                 "synonyms: %zu corpus (>=%u, %zu co-occ dropped) + %zu LRSPL variants -> %zu total -> %s\n",
                 n_corpus, min_support, cooc.size(), n_lex, merged.size(), out_tsv.c_str());
    return true;
}

}  // namespace indicator
}  // namespace mirobody
