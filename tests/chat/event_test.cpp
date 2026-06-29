#include "chat/event/event.hpp"

#include <rapidjson/document.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using mirobody::chat::EndEvent;
using mirobody::chat::UploadEvent;
using mirobody::chat::ReplyEvent;
using mirobody::chat::TranscriptEvent;
using mirobody::chat::ConversationEvent;

namespace {

rapidjson::Document parse(const std::string& json) {
    rapidjson::Document d;
    d.Parse(json.c_str());
    return d;
}

}   // namespace

TEST_CASE("UploadEvent carries its reference as a nested file object", "[chat][event]") {
    UploadEvent ev("report.pdf", "application/pdf",
                       "https://cdn.example/u/abc/report.pdf", "u/abc/report.pdf");

    CHECK(std::string(ev.type()) == "upload");

    rapidjson::Document d = parse(ev.to_json());
    REQUIRE_FALSE(d.HasParseError());
    REQUIRE(d.IsObject());
    CHECK(std::string(d["type"].GetString())    == "upload");
    CHECK(std::string(d["content"].GetString()) == "report.pdf");

    REQUIRE(d.HasMember("file"));
    const rapidjson::Value& f = d["file"];
    REQUIRE(f.IsObject());
    CHECK(std::string(f["filename"].GetString())  == "report.pdf");
    CHECK(std::string(f["mime_type"].GetString()) == "application/pdf");
    CHECK(std::string(f["url"].GetString())       == "https://cdn.example/u/abc/report.pdf");
    CHECK(std::string(f["file_key"].GetString())  == "u/abc/report.pdf");
}

TEST_CASE("TranscriptEvent begin omits the extracted flag", "[chat][event]") {
    TranscriptEvent ev(TranscriptEvent::Phase::Begin, "scan.png");

    CHECK(std::string(ev.type()) == "transcript");

    rapidjson::Document d = parse(ev.to_json());
    REQUIRE_FALSE(d.HasParseError());
    CHECK(std::string(d["type"].GetString())    == "transcript");
    CHECK(std::string(d["content"].GetString()) == "scan.png");
    CHECK(std::string(d["phase"].GetString())   == "begin");
    CHECK_FALSE(d.HasMember("extracted"));
}

TEST_CASE("TranscriptEvent done reports whether text was extracted", "[chat][event]") {
    rapidjson::Document hit = parse(
        TranscriptEvent(TranscriptEvent::Phase::Done, "scan.png", true).to_json());
    REQUIRE_FALSE(hit.HasParseError());
    CHECK(std::string(hit["phase"].GetString()) == "done");
    REQUIRE(hit.HasMember("extracted"));
    REQUIRE(hit["extracted"].IsBool());
    CHECK(hit["extracted"].GetBool());

    rapidjson::Document miss = parse(
        TranscriptEvent(TranscriptEvent::Phase::Done, "scan.png", false).to_json());
    REQUIRE(miss.HasMember("extracted"));
    CHECK_FALSE(miss["extracted"].GetBool());
}

TEST_CASE("ConversationEvent carries the thread id as a precise string + a number", "[chat][event]") {
    // A large id (past JS's 2^53 safe-integer range) must survive as a string so
    // the client can echo it back without precision loss.
    const std::int64_t big = 9007199254740993LL;   // 2^53 + 1
    ConversationEvent ev(big);
    CHECK(std::string(ev.type()) == "conversation");

    rapidjson::Document d = parse(ev.to_json());
    REQUIRE_FALSE(d.HasParseError());
    CHECK(std::string(d["type"].GetString())    == "conversation");
    // content is the exact decimal string (precision-safe for the client).
    CHECK(std::string(d["content"].GetString()) == "9007199254740993");
    REQUIRE(d.HasMember("conversation_id"));
    CHECK(d["conversation_id"].GetInt64() == big);
}

TEST_CASE("to_sse wraps to_json in a Server-Sent Events data frame", "[chat][event]") {
    ReplyEvent ev("hi");
    CHECK(ev.to_sse() == "data: " + ev.to_json() + "\n\n");

    // The terminal end marker the frontend waits for.
    CHECK(parse(EndEvent().to_json())["type"].GetString() == std::string("end"));
}
