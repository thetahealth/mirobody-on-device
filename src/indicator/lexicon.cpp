#include "indicator/lexicon.hpp"

#include "indicator/fhir_id.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace mirobody {
namespace indicator {

namespace {

const char kMagic[4] = {'L', 'X', 'C', '1'};   // uncompressed payload
const char kMagicZ[4] = {'L', 'X', 'C', 'Z'};  // zlib-wrapped: "LXCZ" | u64 raw_len | deflate
const uint32_t kVersion = 2;  // v2: per-code record is u8 system (was u64 fhir_id)
const size_t kCodeRec = 1 + 4 + 4 + 4 + 4;  // system + code_off + name_off + rank + spec

// ─── little-endian POD append / read helpers ─────────────────────────

void put_u8(std::vector<char>& b, uint8_t v) { b.push_back(static_cast<char>(v)); }

void put_u32(std::vector<char>& b, uint32_t v) {
    char tmp[4];
    tmp[0] = static_cast<char>(v & 0xFF);
    tmp[1] = static_cast<char>((v >> 8) & 0xFF);
    tmp[2] = static_cast<char>((v >> 16) & 0xFF);
    tmp[3] = static_cast<char>((v >> 24) & 0xFF);
    b.insert(b.end(), tmp, tmp + 4);
}

void put_u64(std::vector<char>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

// Bounds-checked readers over a flat buffer. On overrun they set ok=false and
// return 0 so the caller can bail without UB.
struct Reader {
    const char* p;
    size_t n;
    size_t off;
    bool ok;
    Reader(const char* p_, size_t n_) : p(p_), n(n_), off(0), ok(true) {}

    uint8_t u8() {
        if (!ok || off + 1 > n) { ok = false; return 0; }
        return static_cast<uint8_t>(p[off++]);
    }
    uint32_t u32() {
        if (!ok || off + 4 > n) { ok = false; return 0; }
        uint32_t v = static_cast<unsigned char>(p[off]) |
                     (static_cast<uint32_t>(static_cast<unsigned char>(p[off + 1])) << 8) |
                     (static_cast<uint32_t>(static_cast<unsigned char>(p[off + 2])) << 16) |
                     (static_cast<uint32_t>(static_cast<unsigned char>(p[off + 3])) << 24);
        off += 4;
        return v;
    }
    uint64_t u64() {
        if (!ok || off + 8 > n) { ok = false; return 0; }
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<uint64_t>(static_cast<unsigned char>(p[off + i])) << (8 * i);
        off += 8;
        return v;
    }
};

// Replace TAB/CR/LF with spaces so a display name carrying an embedded newline
// (LOINC CSV multi-line cells) can't break a TSV row.
std::string tsv_safe(const char* s) {
    std::string o(s);
    for (size_t i = 0; i < o.size(); ++i)
        if (o[i] == '\t' || o[i] == '\n' || o[i] == '\r') o[i] = ' ';
    return o;
}

bool read_file(const std::string& path, std::string& buf, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open " + path; return false; }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    if (sz < 0) { std::fclose(f); err = "ftell failed on " + path; return false; }
    std::fseek(f, 0, SEEK_SET);
    buf.resize(static_cast<size_t>(sz));
    size_t got = sz > 0 ? std::fread(&buf[0], 1, static_cast<size_t>(sz), f) : 0;
    std::fclose(f);
    if (got != static_cast<size_t>(sz)) { err = "short read on " + path; return false; }
    return true;
}

}  // namespace

// ─── LexiconBuilder ──────────────────────────────────────────────────

uint32_t LexiconBuilder::intern(const std::string& s) {
    std::unordered_map<std::string, uint32_t>::iterator it = intern_.find(s);
    if (it != intern_.end()) return it->second;
    uint32_t off = static_cast<uint32_t>(blob_.size());
    blob_.insert(blob_.end(), s.begin(), s.end());
    blob_.push_back('\0');
    intern_[s] = off;
    return off;
}

uint32_t LexiconBuilder::add_code(uint8_t system, const std::string& code,
                                  const std::string& name, uint32_t rank_tier,
                                  uint32_t spec_mask) {
    std::string key(1, static_cast<char>(system));
    key += '\t';
    key += code;
    std::unordered_map<std::string, uint32_t>::iterator it = code_index_.find(key);
    if (it != code_index_.end()) return it->second;
    uint32_t idx = static_cast<uint32_t>(codes_.size());
    BCode b;
    b.system = system;
    b.code_off = intern(code);
    b.name_off = intern(name);
    b.rank_tier = rank_tier;
    b.spec_mask = spec_mask;
    codes_.push_back(b);
    code_index_[key] = idx;
    return idx;
}

void LexiconBuilder::add_surface(const std::string& normalized, uint32_t code_idx) {
    if (normalized.empty()) return;
    std::vector<uint32_t>& v = surfaces_[normalized];
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] == code_idx) return;  // dedup
    v.push_back(code_idx);
}

bool LexiconBuilder::find_code(uint8_t system, const std::string& code, uint32_t& out) const {
    std::string key(1, static_cast<char>(system));
    key += '\t';
    key += code;
    std::unordered_map<std::string, uint32_t>::const_iterator it = code_index_.find(key);
    if (it == code_index_.end()) return false;
    out = it->second;
    return true;
}

const std::vector<uint32_t>* LexiconBuilder::lookup(const std::string& normalized) const {
    std::unordered_map<std::string, std::vector<uint32_t> >::const_iterator it =
        surfaces_.find(normalized);
    if (it == surfaces_.end()) return 0;
    return &it->second;
}

bool LexiconBuilder::write(const std::string& path, std::string& err) const {
    // Flatten surfaces into a stable, sorted order (sorted for future mmap
    // binary search; harmless for the v1 hash-map loader).
    std::vector<const std::string*> keys;
    keys.reserve(surfaces_.size());
    for (std::unordered_map<std::string, std::vector<uint32_t> >::const_iterator it =
             surfaces_.begin(); it != surfaces_.end(); ++it)
        keys.push_back(&it->first);
    std::sort(keys.begin(), keys.end(),
              [](const std::string* a, const std::string* b) { return *a < *b; });

    // The blob is const here, so surface strings must already be interned. They
    // are not (only code/name strings were), so intern surface strings into a
    // local copy of the blob for writing.
    std::vector<char> blob = blob_;
    std::unordered_map<std::string, uint32_t> intern = intern_;
    auto intern_local = [&](const std::string& s) -> uint32_t {
        std::unordered_map<std::string, uint32_t>::iterator it = intern.find(s);
        if (it != intern.end()) return it->second;
        uint32_t off = static_cast<uint32_t>(blob.size());
        blob.insert(blob.end(), s.begin(), s.end());
        blob.push_back('\0');
        intern[s] = off;
        return off;
    };

    std::vector<char> surf_sec;
    std::vector<char> post_sec;
    uint32_t n_postings = 0;
    for (size_t i = 0; i < keys.size(); ++i) {
        const std::string& key = *keys[i];
        const std::vector<uint32_t>& posts = surfaces_.find(key)->second;
        put_u32(surf_sec, intern_local(key));
        put_u32(surf_sec, n_postings);
        put_u32(surf_sec, static_cast<uint32_t>(posts.size()));
        for (size_t j = 0; j < posts.size(); ++j) put_u32(post_sec, posts[j]);
        n_postings += static_cast<uint32_t>(posts.size());
    }

    std::vector<char> out;
    out.insert(out.end(), kMagic, kMagic + 4);
    put_u32(out, kVersion);
    put_u32(out, static_cast<uint32_t>(codes_.size()));
    put_u32(out, static_cast<uint32_t>(keys.size()));
    put_u32(out, n_postings);
    put_u64(out, static_cast<uint64_t>(blob.size()));
    for (size_t i = 0; i < codes_.size(); ++i) {
        put_u8(out, codes_[i].system);
        put_u32(out, codes_[i].code_off);
        put_u32(out, codes_[i].name_off);
        put_u32(out, codes_[i].rank_tier);
        put_u32(out, codes_[i].spec_mask);
    }
    out.insert(out.end(), surf_sec.begin(), surf_sec.end());
    out.insert(out.end(), post_sec.begin(), post_sec.end());
    out.insert(out.end(), blob.begin(), blob.end());

    // zlib-wrap: "LXCZ" | u64 raw_len | deflate(out). (out.size() < 4 GB — the
    // uLong one-shot path is fine; chunk if the artifact ever outgrows that.)
    uLong raw_len = static_cast<uLong>(out.size());
    uLongf comp_cap = compressBound(raw_len);
    std::vector<char> comp(comp_cap);
    uLongf comp_len = comp_cap;
    int rc = compress2(reinterpret_cast<Bytef*>(comp.empty() ? 0 : &comp[0]), &comp_len,
                       reinterpret_cast<const Bytef*>(out.empty() ? "" : &out[0]),
                       raw_len, 6);
    if (rc != Z_OK) { err = "zlib compress failed"; return false; }

    std::vector<char> file;
    file.insert(file.end(), kMagicZ, kMagicZ + 4);
    put_u64(file, static_cast<uint64_t>(raw_len));
    file.insert(file.end(), comp.begin(), comp.begin() + comp_len);

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { err = "cannot write " + path; return false; }
    size_t wrote = std::fwrite(&file[0], 1, file.size(), f);
    std::fclose(f);
    if (wrote != file.size()) { err = "short write on " + path; return false; }
    return true;
}

bool LexiconBuilder::write_tsv(const std::string& path, std::string& err) const {
    std::vector<const std::string*> keys;
    keys.reserve(surfaces_.size());
    for (std::unordered_map<std::string, std::vector<uint32_t> >::const_iterator it =
             surfaces_.begin(); it != surfaces_.end(); ++it)
        keys.push_back(&it->first);
    std::sort(keys.begin(), keys.end(),
              [](const std::string* a, const std::string* b) { return *a < *b; });

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { err = "cannot write " + path; return false; }
    std::fputs("surface\tsystem\tcode\tname\n", f);
    for (size_t i = 0; i < keys.size(); ++i) {
        const std::string& key = *keys[i];
        const std::vector<uint32_t>& idxs = surfaces_.find(key)->second;
        for (size_t j = 0; j < idxs.size(); ++j) {
            const BCode& bc = codes_[idxs[j]];
            std::fprintf(f, "%s\t%s\t%s\t%s\n", key.c_str(),
                         system_name(bc.system),
                         tsv_safe(&blob_[bc.code_off]).c_str(),
                         tsv_safe(&blob_[bc.name_off]).c_str());
        }
    }
    std::fclose(f);
    return true;
}

bool LexiconBuilder::write_tsv_by_code(const std::string& path, std::string& err) const {
    // Invert surface -> codes into code -> surfaces (pointers; keys are stable
    // in surfaces_).
    std::vector<std::vector<const std::string*> > surfs(codes_.size());
    for (std::unordered_map<std::string, std::vector<uint32_t> >::const_iterator it =
             surfaces_.begin(); it != surfaces_.end(); ++it)
        for (size_t j = 0; j < it->second.size(); ++j)
            surfs[it->second[j]].push_back(&it->first);

    // Order codes by (system, name, code) for grouped, deterministic review.
    std::vector<uint32_t> order(codes_.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<uint32_t>(i);
    const std::vector<BCode>& C = codes_;
    const std::vector<char>& B = blob_;
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        int sa = C[a].system, sb = C[b].system;
        if (sa != sb) return sa < sb;
        int nc = std::strcmp(&B[C[a].name_off], &B[C[b].name_off]);
        if (nc != 0) return nc < 0;
        return std::strcmp(&B[C[a].code_off], &B[C[b].code_off]) < 0;
    });

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { err = "cannot write " + path; return false; }
    std::fputs("system\tcode\tname\tsurfaces\n", f);
    for (size_t oi = 0; oi < order.size(); ++oi) {
        uint32_t i = order[oi];
        std::vector<const std::string*>& v = surfs[i];
        std::sort(v.begin(), v.end(),
                  [](const std::string* x, const std::string* y) { return *x < *y; });
        std::fprintf(f, "%s\t%s\t%s\t", system_name(C[i].system),
                     tsv_safe(&B[C[i].code_off]).c_str(), tsv_safe(&B[C[i].name_off]).c_str());
        for (size_t k = 0; k < v.size(); ++k) {
            if (k && *v[k] == *v[k - 1]) continue;  // dedup adjacent
            if (k) std::fputs(" | ", f);
            std::fputs(v[k]->c_str(), f);
        }
        std::fputc('\n', f);
    }
    std::fclose(f);
    return true;
}

// ─── Lexicon (reader) ────────────────────────────────────────────────

bool Lexicon::load(const std::string& path, Lexicon& out, std::string& err) {
    std::string buf;
    if (!read_file(path, buf, err)) return false;

    // zlib-wrapped? "LXCZ" | u64 raw_len | deflate -> decompress into buf.
    if (buf.size() >= 12 && std::memcmp(buf.data(), kMagicZ, 4) == 0) {
        Reader hr(buf.data(), buf.size());
        hr.off = 4;
        uint64_t raw_len = hr.u64();
        std::string raw;
        raw.resize(static_cast<size_t>(raw_len));
        uLongf dst_len = static_cast<uLongf>(raw_len);
        int rc = uncompress(reinterpret_cast<Bytef*>(raw_len ? &raw[0] : 0), &dst_len,
                            reinterpret_cast<const Bytef*>(buf.data() + 12),
                            static_cast<uLong>(buf.size() - 12));
        if (rc != Z_OK || dst_len != raw_len) { err = "zlib decompress failed"; return false; }
        buf.swap(raw);
    }

    if (buf.size() < 24 || std::memcmp(buf.data(), kMagic, 4) != 0) {
        err = "bad magic in " + path;
        return false;
    }
    Reader r(buf.data(), buf.size());
    r.off = 4;  // skip magic
    uint32_t version = r.u32();
    if (version != kVersion) { err = "unsupported lexicon version"; return false; }
    uint32_t n_codes = r.u32();
    uint32_t n_surfaces = r.u32();
    uint32_t n_postings = r.u32();
    uint64_t blob_len = r.u64();
    if (!r.ok) { err = "truncated header"; return false; }

    // Compute section offsets.
    size_t codes_off = r.off;
    size_t surf_off = codes_off + static_cast<size_t>(n_codes) * kCodeRec; // u8+u32*4
    size_t post_off = surf_off + static_cast<size_t>(n_surfaces) * 12;     // u32*3
    size_t blob_off = post_off + static_cast<size_t>(n_postings) * 4;
    if (blob_off + blob_len > buf.size()) { err = "truncated artifact"; return false; }
    const char* blob = buf.data() + blob_off;

    auto str_at = [&](uint32_t off) -> std::string {
        if (off >= blob_len) return std::string();
        return std::string(blob + off);  // NUL-terminated within blob
    };

    out.codes_.resize(n_codes);
    {
        Reader cr(buf.data(), buf.size());
        cr.off = codes_off;
        for (uint32_t i = 0; i < n_codes; ++i) {
            CodeMeta& m = out.codes_[i];
            m.system = cr.u8();
            m.code = str_at(cr.u32());
            m.name = str_at(cr.u32());
            m.rank_tier = cr.u32();
            m.spec_mask = cr.u32();
        }
        if (!cr.ok) { err = "truncated code section"; return false; }
    }

    {
        Reader sr(buf.data(), buf.size());
        sr.off = surf_off;
        const char* posts = buf.data() + post_off;
        for (uint32_t i = 0; i < n_surfaces; ++i) {
            uint32_t s_off = sr.u32();
            uint32_t p_start = sr.u32();
            uint32_t p_count = sr.u32();
            if (!sr.ok) { err = "truncated surface section"; return false; }
            std::vector<uint32_t> codes;
            codes.reserve(p_count);
            for (uint32_t j = 0; j < p_count; ++j) {
                size_t at = (static_cast<size_t>(p_start) + j) * 4;
                if (post_off + at + 4 > buf.size()) { err = "bad postings"; return false; }
                const unsigned char* q = reinterpret_cast<const unsigned char*>(posts + at);
                uint32_t ci = q[0] | (q[1] << 8) | (q[2] << 16) |
                              (static_cast<uint32_t>(q[3]) << 24);
                if (ci < n_codes) codes.push_back(ci);
            }
            out.surfaces_[str_at(s_off)] = codes;
        }
    }
    return true;
}

const std::vector<uint32_t>* Lexicon::lookup(const std::string& normalized) const {
    std::unordered_map<std::string, std::vector<uint32_t> >::const_iterator it =
        surfaces_.find(normalized);
    if (it == surfaces_.end()) return 0;
    return &it->second;
}

}  // namespace indicator
}  // namespace mirobody
