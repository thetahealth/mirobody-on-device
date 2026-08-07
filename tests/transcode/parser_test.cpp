#include "transcode/parser.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using mirobody::file::cap_text;
using mirobody::file::kMaxTextBytes;

namespace {

const char* const kMarker = "\n...[truncated]";

// Whether `s` is well-formed UTF-8: every lead byte followed by exactly the
// number of continuation bytes it announces, and nothing left dangling at the
// end. The point of the cut in cap_text is that this still holds afterwards.
bool valid_utf8(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t extra = 0;
        if (c < 0x80) extra = 0;
        else if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else return false;                       // continuation byte as a lead
        if (i + extra >= s.size() && extra > 0) return false;   // truncated tail
        for (std::size_t k = 1; k <= extra; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += extra + 1;
    }
    return true;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// `n` copies of a three-byte CJK character ("中"), so every cut point that is
// not a multiple of three falls inside a character.
std::string cjk(std::size_t n) {
    std::string out;
    for (std::size_t i = 0; i < n; ++i) out += "\xE4\xB8\xAD";
    return out;
}

}  // namespace

//------------------------------------------------------------------------------
// cap_text
//------------------------------------------------------------------------------

TEST_CASE("cap_text leaves text within the bound untouched", "[parser]") {
    REQUIRE(cap_text("") == "");
    REQUIRE(cap_text("short document") == "short document");

    const std::string exact(kMaxTextBytes, 'a');   // at the bound, not over it
    REQUIRE(cap_text(exact) == exact);
}

TEST_CASE("cap_text truncates past the bound and marks the cut", "[parser]") {
    const std::string over(kMaxTextBytes + 1000, 'a');
    const std::string out = cap_text(over);

    REQUIRE(out.size() == kMaxTextBytes + std::string(kMarker).size());
    REQUIRE(ends_with(out, kMarker));
    REQUIRE(out.compare(0, kMaxTextBytes, over, 0, kMaxTextBytes) == 0);
}

TEST_CASE("cap_text never cuts a UTF-8 sequence in half", "[parser]") {
    // kMaxTextBytes is not a multiple of 3, so the bound lands mid-character:
    // the cut has to back off to the character boundary below it.
    REQUIRE(kMaxTextBytes % 3 != 0);

    const std::string out = cap_text(cjk(kMaxTextBytes));   // 3x the bound
    REQUIRE(ends_with(out, kMarker));

    const std::string body = out.substr(0, out.size() - std::string(kMarker).size());
    REQUIRE(valid_utf8(body));
    REQUIRE(body.size() == kMaxTextBytes - (kMaxTextBytes % 3));
    REQUIRE(body.size() % 3 == 0);
}

TEST_CASE("cap_text cuts at the bound when the input is not UTF-8", "[parser]") {
    // A run of continuation bytes has no lead byte to back off to; the cut must
    // stop looking rather than walk the whole buffer back.
    const std::string blob(kMaxTextBytes + 10, '\x80');
    const std::string out = cap_text(blob);

    REQUIRE(out.size() == kMaxTextBytes + std::string(kMarker).size());
    REQUIRE(ends_with(out, kMarker));
}
