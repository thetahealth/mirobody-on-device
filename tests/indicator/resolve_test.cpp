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
    std::string group;
};

// Cases are grouped by `#@ group=<name>` lines, because the three blocks in
// golden.tsv do not measure the same thing and averaging them hides all of it:
// the generated block's recall is true by construction (its label is derived
// from the same English name the alias table maps onto, so only its top-1 is a
// measurement), while the report-spelling block's recall is the honest one and
// needs span matching to move at all. One aggregate number would report the
// first as progress and bury the second.
std::vector<Golden> load_golden(const std::string& path) {
    std::vector<Golden> out;
    std::ifstream f(path.c_str());
    std::string line, group = "ungrouped";
    const std::string tag = "#@ group=";
    while (std::getline(f, line)) {
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        if (line.compare(0, tag.size(), tag) == 0) {
            group = line.substr(tag.size());
            continue;
        }
        if (line.empty() || line[0] == '#') continue;
        std::string::size_type t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        Golden g;
        g.term = line.substr(0, t1);
        std::string::size_type t2 = line.find('\t', t1 + 1);
        g.expected = line.substr(t1 + 1, t2 == std::string::npos ? std::string::npos : t2 - t1 - 1);
        g.group = group;
        if (!g.term.empty() && !g.expected.empty()) out.push_back(g);
    }
    return out;
}

struct Bucket {
    std::string name;
    int n, recall, top1;
    std::string miss_recall, miss_top1;
    Bucket() : n(0), recall(0), top1(0) {}
};

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
    std::vector<Bucket> buckets;
    Bucket total;
    total.name = "TOTAL";
    for (size_t i = 0; i < golden.size(); ++i) {
        const Golden& g = golden[i];
        size_t b = 0;
        while (b < buckets.size() && buckets[b].name != g.group) ++b;
        if (b == buckets.size()) { buckets.push_back(Bucket()); buckets[b].name = g.group; }

        // Full match set (huge top_k) -> did we recall the analyte at all?
        std::vector<ResolveResult> all = r.resolve(g.term, 1000000);
        bool in_set = false;
        for (size_t j = 0; j < all.size(); ++j)
            if (all[j].code == g.expected) { in_set = true; break; }
        bool is_top1 = !all.empty() && all[0].code == g.expected;

        Bucket& k = buckets[b];
        ++k.n;
        ++total.n;
        if (in_set) { ++k.recall; ++total.recall; } else k.miss_recall += " " + g.term;
        if (is_top1) { ++k.top1; ++total.top1; } else k.miss_top1 += " " + g.term;
    }

    std::printf("\n");
    for (size_t b = 0; b <= buckets.size(); ++b) {
        const Bucket& k = (b < buckets.size()) ? buckets[b] : total;
        std::printf("[indicator golden] %-18s N=%3d  recall=%3d (%3.0f%%)  top-1=%3d (%3.0f%%)\n",
                    k.name.c_str(), k.n, k.recall, 100.0 * k.recall / (k.n ? k.n : 1),
                    k.top1, 100.0 * k.top1 / (k.n ? k.n : 1));
    }
    for (size_t b = 0; b < buckets.size(); ++b) {
        if (!buckets[b].miss_recall.empty())
            std::printf("[indicator golden] %s recall misses:%s\n",
                        buckets[b].name.c_str(), buckets[b].miss_recall.c_str());
    }
    std::printf("[indicator golden] top-1 misses:%s\n",
                total.top1 == total.n ? " (none)" : "see per-group lists above / rerun verbose");

    // Ratchets, one per group, because each is limited by a different stage.
    // Raise them as M2 and M3 land; never lower one to make a run pass.
    for (size_t b = 0; b < buckets.size(); ++b) {
        const Bucket& k = buckets[b];
        if (k.name == "hand") {
            CHECK(k.recall * 100 >= k.n * 50);      // M1 already gets clean names
            CHECK(k.top1 * 100 >= k.n * 65);        // 10% -> 57% M3a -> 76% M3b
        } else if (k.name == "ranking") {
            // Recall here is true by construction -- see load_golden. Only the
            // ordering is a measurement, and it is the one M3a moved: 5% -> 82%.
            CHECK(k.top1 * 100 >= k.n * 90);   // 94% once qualified components sank
        } else if (k.name == "report-spelling") {
            // Was 1/35 before M2a's span fallback and 35/35 after -- the whole
            // point of the group. Floored just under that: every leaf here is a
            // word the lexicon already had, so anything below this means the
            // field split regressed, not that the vocabulary did.
            CHECK(k.recall * 100 >= k.n * 90);
            CHECK(k.top1 * 100 >= k.n * 95);        // 100%: role, then category, then rank
        }
    }
}
