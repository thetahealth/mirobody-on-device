#pragma once

// Word-token vocabulary: tokenize display names into word units and build a
// frequency vocabulary. Reusable by the (future) substring matcher and by the
// `words` review phase. Separated from normalize so the tokenizer can grow
// (n-grams, CJK handling) without touching surface normalization.

#include <cstdint>
#include <string>
#include <vector>

namespace mirobody {
namespace indicator {

// Split into word tokens: normalize() first, then break on every ASCII
// non-alphanumeric byte (space, punctuation, brackets, slash, …). Runs of ASCII
// [a-z0-9] and runs of non-ASCII bytes (CJK etc.) each form a token. Raw — no
// stopword/number filtering (the matcher needs "HPV 16", "vitamin D", "钙").
std::vector<std::string> word_tokens(const std::string& text);

// Build a word-token frequency vocabulary from column `col` (1-based) of a TSV
// (e.g. the build-lexicon --dump output; col 4 = name). Tokenizes via
// word_tokens(), drops pure-number tokens and a small stopword set (single
// letters are KEPT), counts, writes `word<TAB>count` sorted by count desc.
bool build_word_vocab(const std::string& in_tsv, const std::string& out_tsv,
                      int col, uint32_t min_count, std::string& err);

// Mine word-level synonyms from a build-lexicon dump (surface, system, code,
// name). Surfaces sharing a (system, code) are synonymous phrases; two whose
// token SETS are equal-size and differ by exactly one token vote for that token
// pair being synonyms (glucose/葡萄糖, tumor/tumour, hcl/hydrochloride). Votes
// accumulate across concepts; pairs with support >= min_support are kept.
// Noise controls: identifier surfaces (a lone token left after dropping a pure
// number, e.g. "refchem:100") are skipped; [bracketed] packaging segments and a
// curated stoplist of catalog-metadata tokens (RxNorm nomenclature tags like
// inn/usan, store brands like cvs/walgreens) are dropped so they never anchor a
// pair; surfaces over max_tokens (long IUPAC blobs) are skipped; concepts over
// max_group distinct surfaces are skipped; a pair is dropped if its two words
// ever co-occur in one surface (compound, not synonym) or if it is the same
// unit at a different magnitude (1000ug/100ug — same after the leading digits). If lex_dir is non-empty, single-token spelling variants from the
// SPECIALIST Lexicon LRSPL there are merged in verbatim (bypassing the support
// and co-occurrence gates — curated, not voted). Writes
// `word_a<TAB>word_b<TAB>support<TAB>source` (source = corpus | lex |
// corpus+lex) sorted by support desc.
bool build_synonyms(const std::string& in_tsv, const std::string& out_tsv,
                    uint32_t min_support, size_t max_tokens, size_t max_group,
                    const std::string& lex_dir, std::string& err);

}  // namespace indicator
}  // namespace mirobody
