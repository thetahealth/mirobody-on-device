#include "indicator/normalize.hpp"

#include <catch2/catch_test_macros.hpp>

using mirobody::indicator::normalize;

TEST_CASE("normalize casefolds ASCII + collapses whitespace", "[indicator][normalize]") {
    REQUIRE(normalize("Platelet Count") == "platelet count");
    REQUIRE(normalize("  HbA1c  ") == "hba1c");
    REQUIRE(normalize("blood\t glucose\n") == "blood glucose");
    REQUIRE(normalize("Anti-HCV") == "anti-hcv");
}

TEST_CASE("normalize folds full-width + ideographic space", "[indicator][normalize]") {
    REQUIRE(normalize(u8"ＰＬＴ") == "plt");          // full-width latin
    REQUIRE(normalize(u8"血　糖") == u8"血 糖");       // ideographic space -> ASCII space
}

TEST_CASE("normalize folds superscript digits", "[indicator][normalize]") {
    REQUIRE(normalize(u8"10⁹/L") == "109/l");
}

TEST_CASE("normalize preserves CJK and is idempotent", "[indicator][normalize]") {
    std::string once = normalize(u8"血小板");
    REQUIRE(once == u8"血小板");
    REQUIRE(normalize(once) == once);
}

TEST_CASE("normalize returns empty for blank input", "[indicator][normalize]") {
    REQUIRE(normalize("").empty());
    REQUIRE(normalize("   \t\n").empty());
}
