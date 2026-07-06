#include "indicator/sources.hpp"

#include "indicator/fhir_id.hpp"
#include "indicator/normalize.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace mirobody {
namespace indicator {

namespace {

// ─── file / CSV / RRF / gz readers ───────────────────────────────────

bool read_file(const std::string& path, std::string& buf) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    if (sz < 0) { std::fclose(f); return false; }
    std::fseek(f, 0, SEEK_SET);
    buf.resize(static_cast<size_t>(sz));
    size_t got = sz > 0 ? std::fread(&buf[0], 1, static_cast<size_t>(sz), f) : 0;
    std::fclose(f);
    return got == static_cast<size_t>(sz);
}

// ─── directory globbing (version-proof reference paths) ─────────────

struct DirEntry { std::string name; bool is_dir; };

std::vector<DirEntry> list_dir(const std::string& dir) {
    std::vector<DirEntry> out;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    std::string pat = dir + "\\*";
    HANDLE h = FindFirstFileA(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::string n = fd.cFileName;
        if (n == "." || n == "..") continue;
        DirEntry e;
        e.name = n;
        e.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        out.push_back(e);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    struct dirent* de;
    while ((de = readdir(d)) != 0) {
        std::string n = de->d_name;
        if (n == "." || n == "..") continue;
        DirEntry e;
        e.name = n;
        e.is_dir = (de->d_type == DT_DIR);
        if (de->d_type == DT_UNKNOWN) {  // some filesystems don't fill d_type
            struct stat st;
            if (stat((dir + "/" + n).c_str(), &st) == 0) e.is_dir = S_ISDIR(st.st_mode);
        }
        out.push_back(e);
    }
    closedir(d);
#endif
    return out;
}

// Entry (dir if want_dir, else file) under `dir` matching `pattern`; if several,
// the lexicographically largest (newest version, e.g. Loinc_2.83 > Loinc_2.82).
// "" if none. want_dir excludes same-named archives (Loinc_2.82.zip).
std::string find_match(const std::string& dir, const std::string& pattern, bool want_dir) {
    std::regex re(pattern, std::regex::icase);
    std::vector<DirEntry> es = list_dir(dir);
    std::string best;
    for (size_t i = 0; i < es.size(); ++i)
        if (es[i].is_dir == want_dir && std::regex_search(es[i].name, re) && es[i].name > best)
            best = es[i].name;
    return best.empty() ? std::string() : dir + "/" + best;
}

// CSV: quoted fields, "" escape, embedded newlines, leading BOM strip.
class CsvReader {
public:
    explicit CsvReader(const std::string& data) : d_(data), i_(0) {
        if (d_.size() >= 3 && static_cast<unsigned char>(d_[0]) == 0xEF &&
            static_cast<unsigned char>(d_[1]) == 0xBB &&
            static_cast<unsigned char>(d_[2]) == 0xBF)
            i_ = 3;
    }
    bool next(std::vector<std::string>& row) {
        row.clear();
        if (i_ >= d_.size()) return false;
        std::string field;
        bool in_quotes = false, any = false;
        while (i_ < d_.size()) {
            char c = d_[i_];
            any = true;
            if (in_quotes) {
                if (c == '"') {
                    if (i_ + 1 < d_.size() && d_[i_ + 1] == '"') { field.push_back('"'); i_ += 2; }
                    else { in_quotes = false; ++i_; }
                } else { field.push_back(c); ++i_; }
            } else {
                if (c == '"') { in_quotes = true; ++i_; }
                else if (c == ',') { row.push_back(field); field.clear(); ++i_; }
                else if (c == '\r') { ++i_; }
                else if (c == '\n') { ++i_; break; }
                else { field.push_back(c); ++i_; }
            }
        }
        row.push_back(field);
        return any;
    }
private:
    const std::string& d_;
    size_t i_;
};

int col(const std::vector<std::string>& header, const char* name) {
    for (size_t i = 0; i < header.size(); ++i)
        if (header[i] == name) return static_cast<int>(i);
    return -1;
}
const std::string& field(const std::vector<std::string>& row, int idx) {
    static const std::string empty;
    if (idx < 0 || idx >= static_cast<int>(row.size())) return empty;
    return row[idx];
}

// RRF: pipe-delimited, one record per line.
template <typename F>
bool stream_rrf(const std::string& path, F fn) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) return false;
    std::string line;
    std::vector<std::string> f;
    while (std::getline(in, line)) {
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        f.clear();
        size_t start = 0;
        for (size_t i = 0; i <= line.size(); ++i)
            if (i == line.size() || line[i] == '|') { f.push_back(line.substr(start, i - start)); start = i + 1; }
        fn(f);
    }
    return true;
}

template <typename F>
bool gz_lines(const std::string& path, F fn) {
    gzFile g = gzopen(path.c_str(), "rb");
    if (!g) return false;
    std::vector<char> buf(1 << 16);
    while (gzgets(g, &buf[0], static_cast<int>(buf.size()))) {
        std::string line(&buf[0]);
        while (!line.empty() && (line[line.size() - 1] == '\n' || line[line.size() - 1] == '\r'))
            line.erase(line.size() - 1);
        fn(line);
    }
    gzclose(g);
    return true;
}

template <typename F>
bool text_lines(const std::string& path, F fn) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        fn(line);
    }
    return true;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

std::string obo_quoted(const std::string& s) {
    size_t a = s.find('"');
    if (a == std::string::npos) return std::string();
    size_t b = s.find('"', a + 1);
    if (b == std::string::npos) return std::string();
    return s.substr(a + 1, b - a - 1);
}

// ─── surface / anchor helpers ────────────────────────────────────────

// Whole normalized form + first dot-segment (LOINC COMPONENTs are dotted).
void add_surfaces(LexiconBuilder& b, uint32_t idx, const std::string& raw) {
    if (raw.empty()) return;
    std::string norm = normalize(raw);
    if (!norm.empty()) b.add_surface(norm, idx);
    size_t dot = raw.find('.');
    if (dot != std::string::npos && dot > 0) {
        std::string seg = normalize(raw.substr(0, dot));
        if (!seg.empty() && seg != norm) b.add_surface(seg, idx);
    }
}

bool name_anchor(LexiconBuilder& b, const std::string& canonical, uint32_t& out) {
    const std::vector<uint32_t>* c = b.lookup(normalize(canonical));
    if (!c || c->empty()) return false;
    out = (*c)[0];
    return true;
}

void attach_concept(LexiconBuilder& b, uint8_t src_system, const std::string& native_id,
                    const std::string& canonical, const std::vector<std::string>& syns,
                    bool anchored, uint32_t anchor) {
    if (canonical.empty()) return;
    uint32_t idx = anchored ? anchor : b.add_code(src_system, native_id, canonical);
    std::string s = normalize(canonical);
    if (!s.empty()) b.add_surface(s, idx);
    for (size_t i = 0; i < syns.size(); ++i) {
        s = normalize(syns[i]);
        if (!s.empty()) b.add_surface(s, idx);
    }
}

// ─── LOINC ───────────────────────────────────────────────────────────

int build_loinc_core(LexiconBuilder& b, const std::string& path) {
    std::string data;
    if (!read_file(path, data)) {
        std::fprintf(stderr, "build-lexicon: cannot read %s\n", path.c_str());
        return -1;
    }
    CsvReader csv(data);
    std::vector<std::string> header;
    if (!csv.next(header)) return 0;
    int c_num = col(header, "LOINC_NUM"), c_comp = col(header, "COMPONENT"),
        c_lcn = col(header, "LONG_COMMON_NAME"), c_sn = col(header, "SHORTNAME");
    if (c_num < 0) { std::fprintf(stderr, "build-lexicon: no LOINC_NUM in %s\n", path.c_str()); return -1; }
    int n = 0;
    std::vector<std::string> row;
    while (csv.next(row)) {
        const std::string& code = field(row, c_num);
        if (code.empty()) continue;
        const std::string& lcn = field(row, c_lcn);
        const std::string& comp = field(row, c_comp);
        uint32_t idx = b.add_code(SYS_LOINC, code, !lcn.empty() ? lcn : comp);
        add_surfaces(b, idx, lcn);
        add_surfaces(b, idx, comp);
        add_surfaces(b, idx, field(row, c_sn));
        ++n;
    }
    return n;
}

int build_loinc_zh(LexiconBuilder& b, const std::string& path) {
    std::string data;
    if (!read_file(path, data)) {
        std::fprintf(stderr, "build-lexicon: (optional) zhCN variant not read: %s\n", path.c_str());
        return 0;
    }
    CsvReader csv(data);
    std::vector<std::string> header;
    if (!csv.next(header)) return 0;
    int c_num = col(header, "LOINC_NUM"), c_comp = col(header, "COMPONENT"),
        c_lcn = col(header, "LONG_COMMON_NAME");
    if (c_num < 0) return 0;
    int n = 0;
    std::vector<std::string> row;
    while (csv.next(row)) {
        const std::string& code = field(row, c_num);
        if (code.empty()) continue;
        const std::string& comp = field(row, c_comp);
        const std::string& lcn = field(row, c_lcn);
        uint32_t idx = b.add_code(SYS_LOINC, code, !comp.empty() ? comp : lcn);
        add_surfaces(b, idx, comp);
        add_surfaces(b, idx, lcn);
        ++n;
    }
    return n;
}

// ─── UMLS MRCONSO (all-SAB CUI bridge + UMLS_CUI fallback) ───────────

int build_umls(LexiconBuilder& b, const std::string& mrconso) {
    const size_t kMaxCodesPerCui = 16;
    auto native_sys = [](const std::string& sab) -> int {
        if (sab == "SNOMEDCT_US") return SYS_SNOMED_CT;
        if (sab == "LNC") return SYS_LOINC;
        if (sab == "RXNORM") return SYS_RXNORM;
        return -1;
    };
    auto keep_lang = [](const std::string& lat) { return lat == "ENG" || lat == "CHI"; };

    std::unordered_map<uint64_t, std::string> best_name, code_str;
    std::unordered_map<uint64_t, int> best_score;
    std::unordered_map<std::string, std::vector<uint64_t> > cui2fid;
    std::unordered_map<std::string, std::string> cui_name;
    std::unordered_map<std::string, int> cui_score;

    bool ok = stream_rrf(mrconso, [&](const std::vector<std::string>& f) {
        if (f.size() < 17) return;
        const std::string& cui = f[0];
        int sc = (f[1] == "ENG") ? (f[6] == "Y" ? 3 : 2) : 1;
        std::unordered_map<std::string, int>::iterator cs = cui_score.find(cui);
        if (cs == cui_score.end() || sc > cs->second) { cui_score[cui] = sc; cui_name[cui] = f[14]; }
        int sys = native_sys(f[11]);
        if (sys < 0) { cui2fid[cui]; return; }
        uint64_t fid;
        if (!code_to_fhir_id(sys, f[13], fid)) { cui2fid[cui]; return; }
        std::unordered_map<uint64_t, int>::iterator it = best_score.find(fid);
        if (it == best_score.end() || sc > it->second) {
            best_score[fid] = sc; best_name[fid] = f[14]; code_str[fid] = f[13];
        }
        std::vector<uint64_t>& v = cui2fid[cui];
        if (v.size() < kMaxCodesPerCui && std::find(v.begin(), v.end(), fid) == v.end())
            v.push_back(fid);
    });
    if (!ok) {
        std::fprintf(stderr, "build-lexicon: (optional) MRCONSO not read: %s\n", mrconso.c_str());
        return 0;
    }

    std::unordered_map<uint64_t, uint32_t> fid2idx;
    for (std::unordered_map<uint64_t, std::string>::iterator it = best_name.begin();
         it != best_name.end(); ++it)
        fid2idx[it->first] = b.add_code(static_cast<uint8_t>(fhir_id_system(it->first)),
                                        code_str[it->first], it->second);

    std::unordered_map<std::string, std::vector<uint32_t> > cui2idx;
    size_t fallback_codes = 0;
    for (std::unordered_map<std::string, std::vector<uint64_t> >::iterator it =
             cui2fid.begin(); it != cui2fid.end(); ++it) {
        std::vector<uint32_t>& dst = cui2idx[it->first];
        for (size_t i = 0; i < it->second.size(); ++i) {
            std::unordered_map<uint64_t, uint32_t>::iterator j = fid2idx.find(it->second[i]);
            if (j != fid2idx.end()) dst.push_back(j->second);
        }
        if (dst.empty()) { dst.push_back(b.add_code(SYS_CUI, it->first, cui_name[it->first])); ++fallback_codes; }
    }
    std::fprintf(stderr, "build-lexicon: MRCONSO concepts=%zu (UMLS_CUI fallback codes=%zu)\n",
                 cui2idx.size(), fallback_codes);

    int n = 0;
    stream_rrf(mrconso, [&](const std::vector<std::string>& f) {
        if (f.size() < 17 || !keep_lang(f[1])) return;
        std::unordered_map<std::string, std::vector<uint32_t> >::iterator it = cui2idx.find(f[0]);
        if (it == cui2idx.end()) return;
        std::string s = normalize(f[14]);
        if (s.empty()) return;
        for (size_t i = 0; i < it->second.size(); ++i) b.add_surface(s, it->second[i]);
        ++n;
    });
    return n;
}

// ─── CVX ─────────────────────────────────────────────────────────────

int build_cvx(LexiconBuilder& b, const std::string& cvx_txt, const std::string& cvx_cn) {
    int n = 0;
    std::string data;
    if (read_file(cvx_txt, data)) {
        size_t start = 0;
        for (size_t i = 0; i <= data.size(); ++i) {
            if (i != data.size() && data[i] != '\n') continue;
            std::string line = data.substr(start, i - start);
            start = i + 1;
            if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
            if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line = line.substr(3);
            std::vector<std::string> f;
            size_t s2 = 0;
            for (size_t j = 0; j <= line.size(); ++j)
                if (j == line.size() || line[j] == '|') { f.push_back(line.substr(s2, j - s2)); s2 = j + 1; }
            if (f.size() < 3) continue;
            std::string code = trim(f[0]);
            if (code.empty()) continue;
            std::string sn = trim(f[1]), fn = trim(f[2]);
            uint32_t idx = b.add_code(SYS_CVX, code, !fn.empty() ? fn : sn);
            add_surfaces(b, idx, sn);
            add_surfaces(b, idx, fn);
            ++n;
        }
    }
    std::string cn;
    if (read_file(cvx_cn, cn)) {
        CsvReader csv(cn);
        std::vector<std::string> header;
        if (csv.next(header)) {
            int c_code = col(header, "cvx_code"), c_name = col(header, "name_cn");
            std::vector<std::string> row;
            while (csv.next(row)) {
                std::string code = trim(field(row, c_code));
                if (code.empty()) continue;
                uint32_t idx = b.add_code(SYS_CVX, code, field(row, c_name));
                add_surfaces(b, idx, field(row, c_name));
            }
        }
    }
    return n;
}

// ─── foreign-language aliases (colloquial -> English -> code) ────────

bool has_cjk(const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0xE3 && c <= 0xE9) return true;
    }
    return false;
}
bool has_ascii_run(const std::string& s, int n) {
    int r = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) { if (++r >= n) return true; }
        else r = 0;
    }
    return false;
}
bool is_self_pair(const std::string& s) {
    static const char* seps[2] = {" & ", " and "};
    for (int k = 0; k < 2; ++k) {
        std::string sep = seps[k];
        std::string::size_type p = s.find(sep);
        if (p != std::string::npos && s.substr(0, p) == s.substr(p + sep.size())) return true;
    }
    return false;
}

int build_aliases(LexiconBuilder& b, const std::string& path) {
    std::string data;
    if (!read_file(path, data)) {
        std::fprintf(stderr, "build-lexicon: (optional) aliases not read: %s\n", path.c_str());
        return 0;
    }
    int n = 0;
    size_t dropped = 0, start = 0;
    for (size_t i = 0; i <= data.size(); ++i) {
        if (i != data.size() && data[i] != '\n') continue;
        std::string line = data.substr(start, i - start);
        start = i + 1;
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        if (line.empty() || line[0] == '#') continue;
        std::string::size_type tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string src_raw = line.substr(0, tab), dst_raw = line.substr(tab + 1);
        if (is_self_pair(dst_raw) || (has_cjk(src_raw) && has_ascii_run(src_raw, 4))) { ++dropped; continue; }
        std::string src = normalize(src_raw), dst = normalize(dst_raw);
        if (src.empty() || dst.empty()) continue;
        const std::vector<uint32_t>* codes = b.lookup(dst);
        if (!codes) continue;
        for (size_t k = 0; k < codes->size(); ++k) b.add_surface(src, (*codes)[k]);
        ++n;
    }
    if (dropped) std::fprintf(stderr, "build-lexicon: alias junk dropped=%zu\n", dropped);
    return n;
}

// ─── external sources (ChEBI / MONDO / taxdump / LPSN / PubChem) ─────

int build_chebi(LexiconBuilder& b, const std::string& path) {
    int n = 0;
    std::string id, name;
    std::vector<std::string> syns;
    bool in_term = false, obsolete = false;
    struct Flusher {
        LexiconBuilder& b; int& n; std::string& id; std::string& name;
        std::vector<std::string>& syns; bool& in_term; bool& obsolete;
        void operator()() {
            if (in_term && !obsolete && !id.empty() && !name.empty()) {
                uint32_t anc; bool a = name_anchor(b, name, anc);
                attach_concept(b, SYS_CHEBI, id, name, syns, a, anc);
                ++n;
            }
            id.clear(); name.clear(); syns.clear(); obsolete = false;
        }
    } flush = {b, n, id, name, syns, in_term, obsolete};
    bool ok = gz_lines(path, [&](const std::string& line) {
        if (line == "[Term]") { flush(); in_term = true; return; }
        if (!in_term) return;
        if (line.compare(0, 4, "id: ") == 0) id = line.substr(4);
        else if (line.compare(0, 6, "name: ") == 0) name = line.substr(6);
        else if (line.compare(0, 9, "synonym: ") == 0) { std::string q = obo_quoted(line); if (!q.empty()) syns.push_back(q); }
        else if (line.compare(0, 13, "is_obsolete: ") == 0 && line.find("true") != std::string::npos) obsolete = true;
    });
    if (!ok) { std::fprintf(stderr, "build-lexicon: (optional) ChEBI not read: %s\n", path.c_str()); return 0; }
    flush();
    return n;
}

int build_mondo(LexiconBuilder& b, const std::string& path) {
    int n = 0;
    std::string id, name;
    std::vector<std::string> syns;
    std::vector<std::pair<std::string, std::string> > xrefs;
    bool in_term = false, obsolete = false;
    struct Flusher {
        LexiconBuilder& b; int& n; std::string& id; std::string& name;
        std::vector<std::string>& syns; std::vector<std::pair<std::string, std::string> >& xrefs;
        bool& in_term; bool& obsolete;
        void operator()() {
            if (in_term && !obsolete && !id.empty() && !name.empty()) {
                bool a = false; uint32_t anc = 0;
                for (size_t i = 0; i < xrefs.size() && !a; ++i) {
                    const std::string& pfx = xrefs[i].first;
                    if (pfx == "UMLS") a = b.find_code(SYS_CUI, xrefs[i].second, anc);
                    else if (pfx == "SNOMEDCT_US" || pfx == "SCTID" || pfx == "SNOMEDCT")
                        a = b.find_code(SYS_SNOMED_CT, xrefs[i].second, anc);
                }
                if (!a) a = name_anchor(b, name, anc);
                attach_concept(b, SYS_MONDO, id, name, syns, a, anc);
                ++n;
            }
            id.clear(); name.clear(); syns.clear(); xrefs.clear(); obsolete = false;
        }
    } flush = {b, n, id, name, syns, xrefs, in_term, obsolete};
    bool ok = text_lines(path, [&](const std::string& line) {
        if (line == "[Term]") { flush(); in_term = true; return; }
        if (!in_term) return;
        if (line.compare(0, 4, "id: ") == 0) id = line.substr(4);
        else if (line.compare(0, 6, "name: ") == 0) name = line.substr(6);
        else if (line.compare(0, 9, "synonym: ") == 0) { std::string q = obo_quoted(line); if (!q.empty()) syns.push_back(q); }
        else if (line.compare(0, 6, "xref: ") == 0) {
            std::string x = line.substr(6);
            size_t sp = x.find_first_of(" \t{");
            if (sp != std::string::npos) x = x.substr(0, sp);
            size_t colon = x.find(':');
            if (colon != std::string::npos) xrefs.push_back(std::make_pair(x.substr(0, colon), x.substr(colon + 1)));
        } else if (line.compare(0, 13, "is_obsolete: ") == 0 && line.find("true") != std::string::npos) obsolete = true;
    });
    if (!ok) { std::fprintf(stderr, "build-lexicon: (optional) MONDO not read: %s\n", path.c_str()); return 0; }
    flush();
    return n;
}

int build_taxdump(LexiconBuilder& b, const std::string& path) {
    int n = 0;
    std::string cur, sci;
    std::vector<std::string> syns;
    struct Flusher {
        LexiconBuilder& b; int& n; std::string& cur; std::string& sci; std::vector<std::string>& syns;
        void operator()() {
            if (!cur.empty() && !sci.empty()) {
                uint32_t anc; bool a = name_anchor(b, sci, anc);
                attach_concept(b, SYS_NCBITAXON, cur, sci, syns, a, anc);
                ++n;
            }
            sci.clear(); syns.clear();
        }
    } flush = {b, n, cur, sci, syns};
    bool ok = text_lines(path, [&](const std::string& line) {
        std::vector<std::string> f;
        size_t start = 0;
        for (;;) {
            size_t sep = line.find("\t|", start);
            f.push_back(line.substr(start, sep == std::string::npos ? std::string::npos : sep - start));
            if (sep == std::string::npos) break;
            start = sep + 2;
            if (start < line.size() && line[start] == '\t') ++start;
        }
        if (f.size() < 4) return;
        const std::string& taxid = f[0];
        const std::string& nm = f[1];
        const std::string& cls = f[3];
        if (taxid != cur) { flush(); cur = taxid; }
        if (cls == "scientific name") sci = nm;
        else if (cls == "synonym" || cls == "equivalent name" || cls == "common name" ||
                 cls == "genbank common name" || cls == "genbank synonym" ||
                 cls == "blast name" || cls == "acronym" || cls == "genbank acronym")
            syns.push_back(nm);
    });
    if (!ok) { std::fprintf(stderr, "build-lexicon: (optional) taxdump not read: %s\n", path.c_str()); return 0; }
    flush();
    return n;
}

int build_lpsn(LexiconBuilder& b, const std::string& path) {
    std::string data;
    if (!read_file(path, data)) {
        std::fprintf(stderr, "build-lexicon: (optional) LPSN not read: %s\n", path.c_str());
        return 0;
    }
    CsvReader csv(data);
    std::vector<std::string> h;
    if (!csv.next(h)) return 0;
    int c_g = col(h, "genus_name"), c_sp = col(h, "sp_epithet"),
        c_ss = col(h, "subsp_epithet"), c_rec = col(h, "record_no");
    int n = 0;
    std::vector<std::string> row;
    while (csv.next(row)) {
        std::string g = field(row, c_g), sp = field(row, c_sp), ss = field(row, c_ss);
        if (g.empty()) continue;
        std::string binom = g;
        if (!sp.empty()) binom += " " + sp;
        if (!ss.empty()) binom += " subsp. " + ss;
        uint32_t anc; bool a = name_anchor(b, binom, anc);
        std::string id = field(row, c_rec);
        if (id.empty()) id = binom;
        attach_concept(b, SYS_LPSN, id, binom, std::vector<std::string>(), a, anc);
        ++n;
    }
    return n;
}

int build_pubchem(LexiconBuilder& b, const std::string& path) {
    int n = 0;
    std::string cur;
    bool have_anchor = false, first_in_group = true;
    uint32_t anchor = 0;
    bool ok = gz_lines(path, [&](const std::string& line) {
        size_t tab = line.find('\t');
        if (tab == std::string::npos) return;
        std::string cid = line.substr(0, tab), syn = line.substr(tab + 1);
        if (cid != cur) { cur = cid; first_in_group = true; have_anchor = false; }
        if (first_in_group) {
            have_anchor = name_anchor(b, syn, anchor);
            first_in_group = false;
            if (have_anchor) ++n;
        }
        if (have_anchor) { std::string s = normalize(syn); if (!s.empty()) b.add_surface(s, anchor); }
    });
    if (!ok) { std::fprintf(stderr, "build-lexicon: (optional) PubChem not read: %s\n", path.c_str()); return 0; }
    return n;
}

// ─── abbreviation harvesting (Part name vs display, for build_units) ─

std::vector<std::string> ws_tokens(const std::string& s) {
    std::vector<std::string> v;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == ' ') { if (!cur.empty()) { v.push_back(cur); cur.clear(); } }
        else cur.push_back(s[i]);
    }
    if (!cur.empty()) v.push_back(cur);
    return v;
}
std::string join_sp(const std::vector<std::string>& v) {
    std::string o;
    for (size_t i = 0; i < v.size(); ++i) { if (i) o.push_back(' '); o += v[i]; }
    return o;
}
void harvest_abbrev(const std::string& name_n, const std::string& disp_n,
                    std::map<std::pair<std::string, std::string>, int>& counts) {
    std::vector<std::string> a = ws_tokens(name_n), b = ws_tokens(disp_n);
    size_t n = a.size(), m = b.size();
    if (n == 0 || m == 0) return;
    std::vector<std::vector<int> > dp(n + 1, std::vector<int>(m + 1, 0));
    for (size_t i = n; i-- > 0;)
        for (size_t j = m; j-- > 0;)
            dp[i][j] = (a[i] == b[j]) ? dp[i + 1][j + 1] + 1 : std::max(dp[i + 1][j], dp[i][j + 1]);
    size_t i = 0, j = 0;
    std::vector<std::string> ga, gb;
    auto strip_spaces = [](const std::string& s) {
        std::string o;
        for (size_t k = 0; k < s.size(); ++k) if (s[k] != ' ') o.push_back(s[k]);
        return o;
    };
    auto flush = [&]() {
        if (!ga.empty() && !gb.empty()) {
            std::string x = join_sp(ga), y = join_sp(gb);
            std::string xs = strip_spaces(x), ys = strip_spaces(y);
            bool prefix = !xs.empty() && xs.size() < ys.size() && ys.compare(0, xs.size(), xs) == 0;
            bool symbol = (x == "&" || x == "+") && y == "and";
            if (prefix || symbol) ++counts[std::make_pair(x, y)];
        }
        ga.clear(); gb.clear();
    };
    while (i < n && j < m) {
        if (a[i] == b[j]) { flush(); ++i; ++j; }
        else if (dp[i + 1][j] >= dp[i][j + 1]) ga.push_back(a[i++]);
        else gb.push_back(b[j++]);
    }
    while (i < n) ga.push_back(a[i++]);
    while (j < m) gb.push_back(b[j++]);
    flush();
}

// ─── reference-path resolution (version-agnostic) ────────────────────

struct RefPaths {
    std::string loinc_core, loinc_zh, part, part_map, mrconso, cvx, cvx_cn,
                chebi, mondo, taxdump, lpsn, pubchem;
};

// Resolve all source paths under `root`, globbing the version-variable dirs/files
// (Loinc_*, umls-*, zhCN*LinguisticVariant.csv, lpsn_gss_*.csv) so new releases
// need no code edits. Fixed subdir layouts (chebi/, mondo/, …) are kept literal.
RefPaths resolve_ref(const std::string& root) {
    RefPaths p;
    std::string loinc = find_match(root, "^Loinc_", true);
    if (!loinc.empty()) {
        p.loinc_core = loinc + "/LoincTableCore/LoincTableCore.csv";
        p.loinc_zh = find_match(loinc + "/AccessoryFiles/LinguisticVariants",
                                "^zhCN.*LinguisticVariant\\.csv$", false);
        p.part = loinc + "/AccessoryFiles/PartFile/Part.csv";
        p.part_map = loinc + "/AccessoryFiles/PartFile/PartRelatedCodeMapping.csv";
    }
    std::string umls = find_match(root, "^umls-", true);
    if (!umls.empty()) p.mrconso = umls + "/META/MRCONSO.RRF";
    p.cvx = root + "/cvx.txt";
    p.cvx_cn = root + "/cvx_cn.csv";
    p.chebi = root + "/chebi/chebi.obo.gz";
    p.mondo = root + "/mondo/mondo.obo";
    p.taxdump = root + "/ncbi_taxonomy/names.dmp";
    p.lpsn = find_match(root + "/lpsn", "^lpsn_gss_.*\\.csv$", false);
    p.pubchem = root + "/pubchem/CID-Synonym-filtered.gz";
    return p;
}

}  // namespace

// ─── public: orchestration ───────────────────────────────────────────

void build_lexicon(LexiconBuilder& b, const BuildOptions& opt) {
    RefPaths p = resolve_ref(opt.ref);

    int n_loinc = build_loinc_core(b, p.loinc_core);
    int n_zh = build_loinc_zh(b, p.loinc_zh);
    int n_cvx = build_cvx(b, p.cvx, p.cvx_cn);
    std::fprintf(stderr, "build-lexicon: LOINC=%d zhCN=%d CVX=%d -> codes=%zu surfaces=%zu\n",
                 n_loinc, n_zh, n_cvx, b.code_count(), b.surface_count());
    int n_umls = build_umls(b, p.mrconso);
    std::fprintf(stderr, "build-lexicon: MRCONSO surface-rows=%d -> codes=%zu surfaces=%zu\n",
                 n_umls, b.code_count(), b.surface_count());
    if (!opt.aliases.empty()) {
        int n_alias = build_aliases(b, opt.aliases);
        std::fprintf(stderr, "build-lexicon: alias pairs applied=%d -> surfaces=%zu\n",
                     n_alias, b.surface_count());
    }
    // Externals — run after the above so they can anchor to existing concepts.
    int n_chebi = build_chebi(b, p.chebi);
    int n_mondo = build_mondo(b, p.mondo);
    int n_tax = build_taxdump(b, p.taxdump);
    int n_lpsn = build_lpsn(b, opt.lpsn.empty() ? p.lpsn : opt.lpsn);
    std::fprintf(stderr, "build-lexicon: ChEBI=%d MONDO=%d taxdump=%d LPSN=%d -> codes=%zu surfaces=%zu\n",
                 n_chebi, n_mondo, n_tax, n_lpsn, b.code_count(), b.surface_count());
    if (opt.pubchem) {
        int n_pc = build_pubchem(b, p.pubchem);
        std::fprintf(stderr, "build-lexicon: PubChem anchored CIDs=%d -> surfaces=%zu\n",
                     n_pc, b.surface_count());
    }
}

// ─── public: unit thesaurus ──────────────────────────────────────────

int build_units(const std::string& ref, const std::string& out, const std::string& abbrev_out) {
    RefPaths p = resolve_ref(ref);
    std::string partfile = p.part, partmap = p.part_map, mrconso = p.mrconso;

    struct Unit {
        std::string type, name, disp;
        std::vector<std::string> syns;
        std::vector<std::string> snomed;
    };
    std::unordered_map<std::string, Unit> units;
    std::vector<std::string> order;

    {
        std::string data;
        if (!read_file(partfile, data)) {
            std::fprintf(stderr, "build-units: cannot read %s\n", partfile.c_str());
            return 1;
        }
        CsvReader csv(data);
        std::vector<std::string> h;
        if (!csv.next(h)) return 1;
        int c_num = col(h, "PartNumber"), c_type = col(h, "PartTypeName"),
            c_name = col(h, "PartName"), c_disp = col(h, "PartDisplayName");
        std::vector<std::string> row;
        while (csv.next(row)) {
            const std::string& lp = field(row, c_num);
            if (lp.empty()) continue;
            Unit u;
            u.type = field(row, c_type); u.name = field(row, c_name); u.disp = field(row, c_disp);
            units[lp] = u;
            order.push_back(lp);
        }
    }

    std::unordered_map<std::string, int> snomed_targets;
    {
        std::string data;
        if (read_file(partmap, data)) {
            CsvReader csv(data);
            std::vector<std::string> h;
            csv.next(h);
            int c_num = col(h, "PartNumber"), c_eid = col(h, "ExtCodeId"),
                c_edisp = col(h, "ExtCodeDisplayName"), c_esys = col(h, "ExtCodeSystem");
            std::vector<std::string> row;
            while (csv.next(row)) {
                std::unordered_map<std::string, Unit>::iterator it = units.find(field(row, c_num));
                if (it == units.end()) continue;
                const std::string& disp = field(row, c_edisp);
                if (!disp.empty()) it->second.syns.push_back(disp);
                if (field(row, c_esys) == "http://snomed.info/sct") {
                    const std::string& code = field(row, c_eid);
                    if (!code.empty()) { it->second.snomed.push_back(code); snomed_targets[code] = 1; }
                }
            }
        }
    }

    std::unordered_map<std::string, std::string> code2cui;
    std::unordered_map<std::string, int> needed_cuis;
    stream_rrf(mrconso, [&](const std::vector<std::string>& f) {
        if (f.size() < 17 || f[11] != "SNOMEDCT_US") return;
        if (snomed_targets.find(f[13]) == snomed_targets.end()) return;
        if (code2cui.find(f[13]) == code2cui.end()) code2cui[f[13]] = f[0];
        needed_cuis[f[0]] = 1;
    });
    std::unordered_map<std::string, std::vector<std::string> > cui_syns;
    stream_rrf(mrconso, [&](const std::vector<std::string>& f) {
        if (f.size() < 17) return;
        if (f[1] != "ENG" && f[1] != "CHI") return;
        if (needed_cuis.find(f[0]) == needed_cuis.end()) return;
        cui_syns[f[0]].push_back(f[14]);
    });

    std::sort(order.begin(), order.end(), [&](const std::string& a, const std::string& b) {
        const Unit& ua = units[a]; const Unit& ub = units[b];
        if (ua.type != ub.type) return ua.type < ub.type;
        if (ua.name != ub.name) return ua.name < ub.name;
        return a < b;
    });

    std::map<std::pair<std::string, std::string>, int> abbrev_counts;
    FILE* fo = std::fopen(out.c_str(), "wb");
    if (!fo) { std::fprintf(stderr, "build-units: cannot write %s\n", out.c_str()); return 1; }
    std::fputs("part_number\ttype\tname\tsynonyms\n", fo);
    size_t total_syn = 0;
    for (size_t i = 0; i < order.size(); ++i) {
        const Unit& u = units[order[i]];
        if (!abbrev_out.empty() && !u.disp.empty())
            harvest_abbrev(normalize(u.name), normalize(u.disp), abbrev_counts);
        std::vector<std::string> all;
        for (size_t k = 0; k < u.syns.size(); ++k) all.push_back(u.syns[k]);
        for (size_t k = 0; k < u.snomed.size(); ++k) {
            std::unordered_map<std::string, std::string>::iterator c = code2cui.find(u.snomed[k]);
            if (c == code2cui.end()) continue;
            std::unordered_map<std::string, std::vector<std::string> >::iterator s = cui_syns.find(c->second);
            if (s == cui_syns.end()) continue;
            for (size_t j = 0; j < s->second.size(); ++j) all.push_back(s->second[j]);
        }
        std::vector<std::string> seen_norm;
        std::string name_key = normalize(u.name);
        if (!name_key.empty()) seen_norm.push_back(name_key);
        std::fprintf(fo, "%s\t%s\t%s\t", order[i].c_str(), u.type.c_str(), u.name.c_str());
        bool first = true;
        for (size_t k = 0; k < all.size(); ++k) {
            std::string key = normalize(all[k]);
            if (key.empty()) continue;
            bool dup = false;
            for (size_t m = 0; m < seen_norm.size(); ++m) if (seen_norm[m] == key) { dup = true; break; }
            if (dup) continue;
            seen_norm.push_back(key);
            if (!first) std::fputs(" | ", fo);
            std::fputs(all[k].c_str(), fo);
            first = false;
            ++total_syn;
        }
        std::fputc('\n', fo);
    }
    std::fclose(fo);
    std::fprintf(stderr, "build-units: %zu units, %zu distinct synonyms -> %s\n",
                 order.size(), total_syn, out.c_str());

    if (!abbrev_out.empty()) {
        std::vector<std::pair<int, std::pair<std::string, std::string> > > rows;
        for (std::map<std::pair<std::string, std::string>, int>::iterator it =
                 abbrev_counts.begin(); it != abbrev_counts.end(); ++it)
            rows.push_back(std::make_pair(it->second, it->first));
        std::sort(rows.begin(), rows.end(),
                  [](const std::pair<int, std::pair<std::string, std::string> >& a,
                     const std::pair<int, std::pair<std::string, std::string> >& b) {
                      if (a.first != b.first) return a.first > b.first;
                      return a.second < b.second;
                  });
        FILE* fa = std::fopen(abbrev_out.c_str(), "wb");
        if (!fa) { std::fprintf(stderr, "build-units: cannot write %s\n", abbrev_out.c_str()); return 1; }
        std::fputs("abbrev\texpansion\tcount\n", fa);
        for (size_t i = 0; i < rows.size(); ++i)
            std::fprintf(fa, "%s\t%s\t%d\n", rows[i].second.first.c_str(),
                         rows[i].second.second.c_str(), rows[i].first);
        std::fclose(fa);
        std::fprintf(stderr, "build-units: %zu abbrev pairs -> %s\n", rows.size(), abbrev_out.c_str());
    }
    return 0;
}

}  // namespace indicator
}  // namespace mirobody
