// Golden-set accuracy harness for the terminology resolver.
//
// Skips unless BOTH env vars are set (the lexicon artifact and golden TSV are
// not committed — building the artifact needs a local reference tree, see
// src/indicator/README.md):
//   INDICATOR_LEXICON  -> path to fhir_lexicon.bin
//   INDICATOR_GOLDEN   -> path to tests/indicator/golden.tsv
//
// Reports two numbers (see golden.tsv header): recall (analyte recognized,
// expected code anywhere in the full match set) and top-1 (expected is #1).
// recall isolates the RECALL stage; top-1 isolates RANKING. The REQUIRE floors
// are ratchets — raise them as each milestone lands.

#include "indicator/lexicon.hpp"
#include "indicator/resolve.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using mirobody::indicator::Lexicon;
using mirobody::indicator::ResolveResult;
using mirobody::indicator::Resolver;

namespace {

struct Golden {
    std::string term;
    std::string expected;
};

std::vector<Golden> load_golden(const std::string& path) {
    std::vector<Golden> out;
    std::ifstream f(path.c_str());
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::string::size_type t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        Golden g;
        g.term = line.substr(0, t1);
        std::string::size_type t2 = line.find('\t', t1 + 1);
        g.expected = line.substr(t1 + 1, t2 == std::string::npos ? std::string::npos : t2 - t1 - 1);
        if (!g.term.empty() && !g.expected.empty()) out.push_back(g);
    }
    return out;
}

const char* env(const char* name) {
    const char* v = std::getenv(name);
    return (v && v[0]) ? v : 0;
}

}  // namespace

TEST_CASE("resolve golden accuracy", "[indicator][resolve][golden]") {
    const char* lex_path = env("INDICATOR_LEXICON");
    const char* gold_path = env("INDICATOR_GOLDEN");
    if (!lex_path || !gold_path)
        SKIP("set INDICATOR_LEXICON and INDICATOR_GOLDEN to run the golden harness");

    Lexicon lex;
    std::string err;
    if (!Lexicon::load(lex_path, lex, err)) SKIP("lexicon load failed: " + err);

    std::vector<Golden> golden = load_golden(gold_path);
    REQUIRE(!golden.empty());

    Resolver r(lex);
    int recall_hits = 0, top1_hits = 0;
    std::string miss_recall, miss_top1;
    for (size_t i = 0; i < golden.size(); ++i) {
        const Golden& g = golden[i];
        // Full match set (huge top_k) -> did we recall the analyte at all?
        std::vector<ResolveResult> all = r.resolve(g.term, 1000000);
        bool in_set = false;
        for (size_t j = 0; j < all.size(); ++j)
            if (all[j].code == g.expected) { in_set = true; break; }
        bool is_top1 = !all.empty() && all[0].code == g.expected;
        if (in_set) ++recall_hits; else miss_recall += " " + g.term;
        if (is_top1) ++top1_hits; else miss_top1 += " " + g.term;
    }

    int n = static_cast<int>(golden.size());
    std::printf("\n[indicator golden] N=%d  recall=%d/%d (%.0f%%)  top-1=%d/%d (%.0f%%)\n",
                n, recall_hits, n, 100.0 * recall_hits / n,
                top1_hits, n, 100.0 * top1_hits / n);
    std::printf("[indicator golden] recall misses:%s\n",
                miss_recall.empty() ? " (none)" : miss_recall.c_str());
    std::printf("[indicator golden] top-1 misses:%s\n",
                miss_top1.empty() ? " (none)" : miss_top1.c_str());

    // M1 ratchet floors (raise as M2/M3 land). Exact recall already recognizes
    // the clean analyte names; top-1 is low until the rerank port (M3).
    CHECK(recall_hits * 100 >= n * 50);  // >= 50% recall at M1
}
