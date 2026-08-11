// indicator — debug CLI for the src/indicator terminology resolver.
//
// Thin front-end: parse args and dispatch. All ingestion/derivation logic lives
// in src/indicator (sources.* / word.* / lexicon.* / resolve.*) so it is reusable
// and testable. Phases chain via files:
//   build-lexicon [--dump vocab_all.tsv]  → fhir_lexicon.bin (+ dumps)
//   words     --in vocab_all.tsv          → vocab_name.tsv       (tokenizer iteration)
//   synonyms  --in vocab_all.tsv          → synonyms.tsv         (word-pair mining)
//   build-units                           → units.tsv (+ --abbrev)
//   resolve <term>...                     → ranked codes (JSON lines)

#include "indicator/fhir_id.hpp"
#include "indicator/lexicon.hpp"
#include "indicator/resolve.hpp"
#include "indicator/sources.hpp"
#include "indicator/word.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32")
#endif

namespace {

using namespace mirobody::indicator;

#ifdef _WIN32
// Windows argv arrives in the ANSI code page, mangling CJK; re-derive UTF-8.
std::vector<std::string> utf8_args() {
    std::vector<std::string> out;
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!w) return out;
    for (int i = 0; i < n; ++i) {
        int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s;
        if (len > 1) {
            s.resize(static_cast<size_t>(len - 1));
            WideCharToMultiByte(CP_UTF8, 0, w[i], -1, &s[0], len, nullptr, nullptr);
        }
        out.push_back(s);
    }
    LocalFree(w);
    return out;
}
#endif

void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage:\n"
        "  %s build-lexicon [--ref DIR] [--out PATH] [--aliases TSV] [--lpsn CSV] [--no-pubchem] [--dump TSV] [--dump-by-code TSV]\n"
        "  %s build-units [--ref DIR] [--out TSV] [--abbrev TSV]\n"
        "  %s words [--in TSV] [--out TSV] [--col N] [--min-count N]\n"
        "  %s synonyms [--in TSV] [--out TSV] [--lex LEX_DIR] [--min-support N] [--max-tokens N] [--max-group N]\n"
        "  %s resolve [--lexicon PATH] [--top-k K] [--systems LOINC,SNOMED_CT] <term>...\n"
        "\n"
        "--ref falls back to $MIROBODY_REF. There is no built-in default: the raw\n"
        "reference releases live outside the repo, wherever you extracted them.\n",
        prog, prog, prog, prog, prog);
}

// The reference-data root is machine-local, so nothing is worth baking in: --ref
// wins, then $MIROBODY_REF, and the build commands refuse to guess past that.
std::string ref_from_env() {
    const char* env = std::getenv("MIROBODY_REF");
    return env ? env : "";
}

bool have_ref(const std::string& ref, const char* cmd) {
    if (!ref.empty()) return true;
    std::fprintf(stderr, "%s: no reference-data root. Pass --ref DIR or set MIROBODY_REF.\n", cmd);
    return false;
}

int cmd_build_lexicon(const std::vector<std::string>& args) {
    BuildOptions opt;
    opt.ref = ref_from_env();
    std::string out = "res/indicator/fhir_lexicon.bin", dump, dump_by_code;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--ref" && i + 1 < args.size()) opt.ref = args[++i];
        else if (args[i] == "--out" && i + 1 < args.size()) out = args[++i];
        else if (args[i] == "--aliases" && i + 1 < args.size()) opt.aliases = args[++i];
        else if (args[i] == "--lpsn" && i + 1 < args.size()) opt.lpsn = args[++i];
        else if (args[i] == "--no-pubchem") opt.pubchem = false;
        else if (args[i] == "--dump" && i + 1 < args.size()) dump = args[++i];
        else if (args[i] == "--dump-by-code" && i + 1 < args.size()) dump_by_code = args[++i];
    }
    if (!have_ref(opt.ref, "build-lexicon")) return 2;

    LexiconBuilder b;
    build_lexicon(b, opt);

    std::string err;
    if (!b.write(out, err)) {
        std::fprintf(stderr, "build-lexicon: write failed: %s\n", err.c_str());
        return 1;
    }
    std::fprintf(stderr, "build-lexicon: wrote %s\n", out.c_str());
    if (!dump.empty()) {
        if (!b.write_tsv(dump, err)) { std::fprintf(stderr, "build-lexicon: dump failed: %s\n", err.c_str()); return 1; }
        std::fprintf(stderr, "build-lexicon: dumped review TSV -> %s\n", dump.c_str());
    }
    if (!dump_by_code.empty()) {
        if (!b.write_tsv_by_code(dump_by_code, err)) { std::fprintf(stderr, "build-lexicon: dump-by-code failed: %s\n", err.c_str()); return 1; }
        std::fprintf(stderr, "build-lexicon: dumped per-code TSV -> %s\n", dump_by_code.c_str());
    }
    return 0;
}

int cmd_build_units(const std::vector<std::string>& args) {
    std::string ref = ref_from_env(), out = "build/units.tsv", abbrev;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--ref" && i + 1 < args.size()) ref = args[++i];
        else if (args[i] == "--out" && i + 1 < args.size()) out = args[++i];
        else if (args[i] == "--abbrev" && i + 1 < args.size()) abbrev = args[++i];
    }
    if (!have_ref(ref, "build-units")) return 2;
    return build_units(ref, out, abbrev);
}

int cmd_words(const std::vector<std::string>& args) {
    std::string in = "build/vocab_all.tsv", out = "build/vocab_name.tsv";
    int coln = 4;
    uint32_t min_count = 1;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--in" && i + 1 < args.size()) in = args[++i];
        else if (args[i] == "--out" && i + 1 < args.size()) out = args[++i];
        else if (args[i] == "--col" && i + 1 < args.size()) coln = std::atoi(args[++i].c_str());
        else if (args[i] == "--min-count" && i + 1 < args.size())
            min_count = static_cast<uint32_t>(std::atoi(args[++i].c_str()));
    }
    std::string err;
    if (!build_word_vocab(in, out, coln, min_count, err)) { std::fprintf(stderr, "words: %s\n", err.c_str()); return 1; }
    return 0;
}

int cmd_synonyms(const std::vector<std::string>& args) {
    std::string in = "build/vocab_all.tsv", out = "build/synonyms.tsv", lex;
    uint32_t min_support = 10;
    size_t max_tokens = 12, max_group = 80;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--in" && i + 1 < args.size()) in = args[++i];
        else if (args[i] == "--out" && i + 1 < args.size()) out = args[++i];
        else if (args[i] == "--lex" && i + 1 < args.size()) lex = args[++i];
        else if (args[i] == "--min-support" && i + 1 < args.size())
            min_support = static_cast<uint32_t>(std::atoi(args[++i].c_str()));
        else if (args[i] == "--max-tokens" && i + 1 < args.size())
            max_tokens = static_cast<size_t>(std::atoi(args[++i].c_str()));
        else if (args[i] == "--max-group" && i + 1 < args.size())
            max_group = static_cast<size_t>(std::atoi(args[++i].c_str()));
    }
    std::string err;
    if (!build_synonyms(in, out, min_support, max_tokens, max_group, lex, err)) {
        std::fprintf(stderr, "synonyms: %s\n", err.c_str());
        return 1;
    }
    return 0;
}

void split_csv_list(const std::string& s, std::vector<std::string>& out) {
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur.push_back(s[i]);
    }
    if (!cur.empty()) out.push_back(cur);
}

// One term per line, blanks and #-comments skipped, so a whole report's worth of
// indicator names can be piped in. "-" reads stdin.
bool read_terms(const std::string& path, std::vector<std::string>& out) {
    FILE* f = (path == "-") ? stdin : std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string line;
    int c;
    while ((c = std::fgetc(f)) != EOF) {
        if (c == '\n') {
            while (!line.empty() && (line[line.size() - 1] == '\r' || line[line.size() - 1] == ' '))
                line.erase(line.size() - 1);
            if (!line.empty() && line[0] != '#') out.push_back(line);
            line.clear();
        } else {
            line.push_back(static_cast<char>(c));
        }
    }
    if (!line.empty() && line[0] != '#') out.push_back(line);
    if (f != stdin) std::fclose(f);
    return true;
}

int cmd_resolve(const std::vector<std::string>& args) {
    std::string lexicon_path = "res/indicator/fhir_lexicon.bin", in_path;
    int top_k = 5;
    bool fhir = false;
    std::vector<std::string> systems, terms;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--lexicon" && i + 1 < args.size()) lexicon_path = args[++i];
        else if (a == "--top-k" && i + 1 < args.size()) top_k = std::atoi(args[++i].c_str());
        else if (a == "--systems" && i + 1 < args.size()) split_csv_list(args[++i], systems);
        else if (a == "--in" && i + 1 < args.size()) in_path = args[++i];
        else if (a == "--fhir") fhir = true;
        else terms.push_back(a);
    }
    if (!in_path.empty() && !read_terms(in_path, terms)) {
        std::fprintf(stderr, "resolve: cannot read %s\n", in_path.c_str());
        return 1;
    }
    if (terms.empty()) { std::fprintf(stderr, "resolve: no terms given\n"); return 2; }

    Lexicon lex;
    std::string err;
    if (!Lexicon::load(lexicon_path, lex, err)) { std::fprintf(stderr, "resolve: %s\n", err.c_str()); return 1; }
    Resolver r(lex);
    for (size_t t = 0; t < terms.size(); ++t) {
        std::vector<ResolveResult> res = r.resolve(terms[t], top_k, systems);
        rapidjson::Document d;
        d.SetObject();
        rapidjson::Document::AllocatorType& a = d.GetAllocator();
        if (fhir) {
            // A FHIR CodeableConcept, which is what a caller actually needs and
            // what makes the miss path explicit: `text` always carries what the
            // report said, `coding` is empty when nothing resolved. An empty
            // coding beside a populated text is valid R4 and is the honest
            // answer for the ~20% of Chinese indicators no standard codes.
            d.AddMember("text", rapidjson::Value(terms[t].c_str(), a), a);
            rapidjson::Value cs(rapidjson::kArrayType);
            for (size_t i = 0; i < res.size(); ++i) {
                rapidjson::Value o(rapidjson::kObjectType);
                const char* url = system_url(system_from_name(res[i].system));
                if (url[0]) o.AddMember("system", rapidjson::Value(url, a), a);
                o.AddMember("code", rapidjson::Value(res[i].code.c_str(), a), a);
                o.AddMember("display", rapidjson::Value(res[i].name.c_str(), a), a);
                cs.PushBack(o, a);
            }
            d.AddMember("coding", cs, a);
            rapidjson::StringBuffer fb;
            rapidjson::Writer<rapidjson::StringBuffer> fw(fb);
            d.Accept(fw);
            std::printf("%s\n", fb.GetString());
            continue;
        }
        d.AddMember("term", rapidjson::Value(terms[t].c_str(), a), a);
        rapidjson::Value arr(rapidjson::kArrayType);
        for (size_t i = 0; i < res.size(); ++i) {
            rapidjson::Value o(rapidjson::kObjectType);
            o.AddMember("system", rapidjson::Value(res[i].system.c_str(), a), a);
            o.AddMember("code", rapidjson::Value(res[i].code.c_str(), a), a);
            o.AddMember("name", rapidjson::Value(res[i].name.c_str(), a), a);
            o.AddMember("score", res[i].score, a);
            arr.PushBack(o, a);
        }
        d.AddMember("results", arr, a);
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        d.Accept(w);
        std::printf("%s\n", buf.GetString());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    (void)argc; (void)argv;
    std::vector<std::string> args = utf8_args();
#else
    std::vector<std::string> args(argv, argv + argc);
#endif
    const char* prog = args.empty() ? "indicator" : args[0].c_str();
    if (args.size() < 2) { print_usage(prog); return 2; }
    std::string cmd = args[1];
    std::vector<std::string> rest(args.begin() + 2, args.end());

    if (cmd == "-h" || cmd == "--help") { print_usage(prog); return 0; }
    if (cmd == "build-lexicon") return cmd_build_lexicon(rest);
    if (cmd == "build-units") return cmd_build_units(rest);
    if (cmd == "words") return cmd_words(rest);
    if (cmd == "synonyms") return cmd_synonyms(rest);
    if (cmd == "resolve") return cmd_resolve(rest);
    print_usage(prog);
    return 2;
}
