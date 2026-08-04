#include "llm/local.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

using mirobody::llm::ThinkSplitter;

namespace {

// One emitted piece: which stream it went to, and its text.
struct Piece {
    bool thinking;
    std::string text;
};

// Feed a whole reply as ONE chunk.
std::vector<Piece> split(const std::string& text) {
    std::vector<Piece> out;
    ThinkSplitter s;
    const ThinkSplitter::Emit collect = [&](bool thinking, const std::string& t) -> bool {
        out.push_back(Piece{thinking, t});
        return true;
    };
    s.feed(text, collect);
    s.flush(collect);
    return out;
}

// Feed the same reply one BYTE at a time — the worst case for a tag getting split
// across chunks, and closer to reality than one big string: llama.cpp hands back a
// piece per token, and a marker is never guaranteed to be one token.
std::vector<Piece> split_bytewise(const std::string& text) {
    std::vector<Piece> out;
    ThinkSplitter s;
    const ThinkSplitter::Emit collect = [&](bool thinking, const std::string& t) -> bool {
        out.push_back(Piece{thinking, t});
        return true;
    };
    for (size_t i = 0; i < text.size(); i++) {
        s.feed(text.substr(i, 1), collect);
    }
    s.flush(collect);
    return out;
}

// Concatenate everything that went to one stream.
std::string joined(const std::vector<Piece>& ps, bool thinking) {
    std::string out;
    for (const Piece& p : ps) {
        if (p.thinking == thinking) { out += p.text; }
    }
    return out;
}

}   // namespace

TEST_CASE("plain text passes through untouched", "[think]") {
    for (const std::vector<Piece>& ps : {split("Hello there"), split_bytewise("Hello there")}) {
        CHECK(joined(ps, false) == "Hello there");
        CHECK(joined(ps, true).empty());
    }
}

TEST_CASE("an empty think block is dropped entirely", "[think]") {
    // Qwen3's non-thinking mode still emits the pair; this is the case that showed
    // up as a literal "<think></think>" in the chat bubble.
    for (const std::vector<Piece>& ps :
         {split("<think></think>Hello! How can I help you today?"),
          split_bytewise("<think></think>Hello! How can I help you today?")}) {
        CHECK(joined(ps, false) == "Hello! How can I help you today?");
        CHECK(joined(ps, true).empty());
    }
}

TEST_CASE("reasoning is routed away from the answer", "[think]") {
    const std::string raw = "<think>let me count: 2+2=4</think>The answer is 4.";
    for (const std::vector<Piece>& ps : {split(raw), split_bytewise(raw)}) {
        CHECK(joined(ps, true) == "let me count: 2+2=4");
        CHECK(joined(ps, false) == "The answer is 4.");
    }
}

TEST_CASE("markers never appear in either stream", "[think]") {
    const std::string raw = "a<think>b</think>c";
    for (const std::vector<Piece>& ps : {split(raw), split_bytewise(raw)}) {
        const std::string all = joined(ps, false) + joined(ps, true);
        CHECK(all.find('<') == std::string::npos);
        CHECK(all.find("think") == std::string::npos);
    }
}

TEST_CASE("text before the opening marker stays in the answer", "[think]") {
    const std::string raw = "Sure. <think>hmm</think>Done.";
    for (const std::vector<Piece>& ps : {split(raw), split_bytewise(raw)}) {
        CHECK(joined(ps, false) == "Sure. Done.");
        CHECK(joined(ps, true) == "hmm");
    }
}

TEST_CASE("several blocks in one reply are all handled", "[think]") {
    const std::string raw = "<think>one</think>A<think>two</think>B";
    for (const std::vector<Piece>& ps : {split(raw), split_bytewise(raw)}) {
        CHECK(joined(ps, true) == "onetwo");
        CHECK(joined(ps, false) == "AB");
    }
}

TEST_CASE("a tag split across chunks is still recognized", "[think]") {
    // The specific failure a whole-string scan would have: no single chunk contains
    // the marker.
    std::vector<Piece> out;
    ThinkSplitter s;
    const ThinkSplitter::Emit collect = [&](bool thinking, const std::string& t) -> bool {
        out.push_back(Piece{thinking, t});
        return true;
    };
    s.feed("<th", collect);
    s.feed("ink>rea", collect);
    s.feed("son</thi", collect);
    s.feed("nk>answer", collect);
    s.flush(collect);

    CHECK(joined(out, true) == "reason");
    CHECK(joined(out, false) == "answer");
}

TEST_CASE("an unterminated think block still yields its text", "[think]") {
    // Hit the token cap mid-reasoning: the words must not vanish.
    const std::string raw = "<think>reasoning that never closes";
    for (const std::vector<Piece>& ps : {split(raw), split_bytewise(raw)}) {
        CHECK(joined(ps, true) == "reasoning that never closes");
        CHECK(joined(ps, false).empty());
    }
}

TEST_CASE("a dangling partial marker is treated as content", "[think]") {
    // "<thi" never became a tag, so it was ordinary text and must be flushed.
    for (const std::vector<Piece>& ps : {split("answer<thi"), split_bytewise("answer<thi")}) {
        CHECK(joined(ps, false) == "answer<thi");
        CHECK(joined(ps, true).empty());
    }
}

TEST_CASE("angle brackets that are not markers survive", "[think]") {
    const std::string raw = "use <div> and 3 < 5 <thinking> too";
    for (const std::vector<Piece>& ps : {split(raw), split_bytewise(raw)}) {
        CHECK(joined(ps, false) == raw);
        CHECK(joined(ps, true).empty());
    }
}

TEST_CASE("an aborting handler stops the feed", "[think]") {
    int calls = 0;
    ThinkSplitter s;
    const ThinkSplitter::Emit refuse = [&](bool, const std::string&) -> bool {
        calls++;
        return false;
    };
    CHECK_FALSE(s.feed("some text", refuse));
    CHECK(calls == 1);
}

TEST_CASE("streaming is not delayed by holding back a whole tag", "[think]") {
    // Only a possible tag PREFIX may be held: text that cannot start a marker has to
    // go out immediately, or the reply would arrive in visible stutters.
    std::vector<Piece> out;
    ThinkSplitter s;
    const ThinkSplitter::Emit collect = [&](bool thinking, const std::string& t) -> bool {
        out.push_back(Piece{thinking, t});
        return true;
    };
    s.feed("hello", collect);
    REQUIRE(out.size() == 1);
    CHECK(out[0].text == "hello");
}

//------------------------------------------------------------------------------
// utf8_complete_len — a token is a byte sequence, not a character
//------------------------------------------------------------------------------

using mirobody::llm::utf8_complete_len;

TEST_CASE("complete text is fully emittable", "[utf8]") {
    CHECK(utf8_complete_len("") == 0);
    CHECK(utf8_complete_len("hello") == 5);
    CHECK(utf8_complete_len("\xe4\xbd\xa0") == 3);            // 你
    CHECK(utf8_complete_len("a\xe4\xbd\xa0" "b") == 5);
}

TEST_CASE("an incomplete trailing character is held back", "[utf8]") {
    // 你 is E4 BD A0. Any prefix of it must be withheld, whole or not at all.
    CHECK(utf8_complete_len("\xe4") == 0);
    CHECK(utf8_complete_len("\xe4\xbd") == 0);
    CHECK(utf8_complete_len("hi\xe4") == 2);
    CHECK(utf8_complete_len("hi\xe4\xbd") == 2);
    CHECK(utf8_complete_len("hi\xe4\xbd\xa0") == 5);
}

TEST_CASE("two- and four-byte sequences are measured correctly", "[utf8]") {
    CHECK(utf8_complete_len("\xc3\xa9") == 2);                // é
    CHECK(utf8_complete_len("\xc3") == 0);
    CHECK(utf8_complete_len("\xf0\x9f\x98\x80") == 4);        // 😀
    CHECK(utf8_complete_len("\xf0\x9f\x98") == 0);
    CHECK(utf8_complete_len("x\xf0\x9f\x98") == 1);
}

TEST_CASE("invalid bytes are passed through rather than stalling", "[utf8]") {
    // A stray continuation byte can never be completed; withholding it forever would
    // wedge the stream, so it goes out and the renderer shows one bad glyph.
    CHECK(utf8_complete_len("\xa0") == 1);
    CHECK(utf8_complete_len("\xff\xfe") == 2);
}

TEST_CASE("a CJK reply split at every byte boundary reassembles exactly", "[utf8]") {
    // The real failure: byte-level BPE routinely splits a 3-byte CJK character across
    // two tokens, so feeding token bytes straight out ships half a character.
    const std::string reply = "你好，世界！这是本地模型。";
    for (size_t cut = 1; cut < reply.size(); cut++) {
        std::string carry;
        std::string emitted;
        const std::string parts[2] = {reply.substr(0, cut), reply.substr(cut)};
        for (const std::string& part : parts) {
            carry += part;
            const size_t whole = utf8_complete_len(carry);
            emitted += carry.substr(0, whole);
            carry.erase(0, whole);
        }
        // Nothing lost, nothing reordered, and no piece ever ended mid-character.
        CHECK(emitted == reply);
        CHECK(carry.empty());
    }
}

TEST_CASE("every emitted piece is independently valid UTF-8", "[utf8]") {
    // What actually matters downstream: each piece crosses NAPI on its own, so a
    // piece ending mid-character is unrecoverable once the host substitutes U+FFFD.
    const std::string reply = "本地模型很快";
    std::string carry;
    for (size_t i = 0; i < reply.size(); i++) {
        carry += reply[i];
        const size_t whole = utf8_complete_len(carry);
        if (whole > 0) {
            const std::string piece = carry.substr(0, whole);
            CHECK(utf8_complete_len(piece) == piece.size());   // self-contained
            carry.erase(0, whole);
        }
    }
    CHECK(carry.empty());
}
