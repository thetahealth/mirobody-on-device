// ChatParams::parse -- the packet -> AgentRequest translation, with the upload
// bytes it overlays onto the file refs.
//
// The point of most of these: an uploaded file must reach the agent WITHOUT
// being copied. parse() reads a const Packet, so an owning buffer on AgentFile
// could only ever be duplicated; the bytes are a mirobody::Blob so the ref and
// the attachment share one buffer. Pointer identity is the assertion -- equal
// contents would also pass if the bytes had been copied, which is the very
// thing these pin down.

#include "chat/params.hpp"

#include "chat/dispatcher.hpp"   // kOpChat
#include "chat/packet.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using mirobody::Blob;
using mirobody::chat::Attachment;
using mirobody::chat::ChatParams;
using mirobody::chat::Packet;
using mirobody::chat::kOpChat;

namespace {

// A packet shaped like one the dispatcher hands to parse(): a question, one
// stored-file reference in params.files, and the raw bytes still attached.
Packet packet_with_upload(const std::string& filename, const std::string& bytes) {
    Packet pkt(kOpChat);
    rapidjson::Document::AllocatorType& a = pkt.allocator();
    rapidjson::Value& p = pkt.params_mut();

    p.AddMember("question", rapidjson::Value("what is this?", a), a);

    rapidjson::Value f(rapidjson::kObjectType);
    f.AddMember("filename", rapidjson::Value(filename.c_str(),
                    static_cast<rapidjson::SizeType>(filename.size()), a), a);
    f.AddMember("mime_type", rapidjson::Value("image/png", a), a);
    f.AddMember("file_key", rapidjson::Value("seg/a/abcdef.png", a), a);
    f.AddMember("url", rapidjson::Value("https://example.test/abcdef.png", a), a);
    rapidjson::Value files(rapidjson::kArrayType);
    files.PushBack(f, a);
    p.AddMember("files", files, a);

    Attachment att;
    att.filename  = filename;
    att.mime_type = "image/png";
    att.data      = Blob(bytes);
    pkt.add_attachment(std::move(att));
    return pkt;
}

}   // namespace

TEST_CASE("parse carries the upload bytes to the matching file ref", "[chat][params]") {
    const std::string bytes(4096, '\x7f');
    Packet pkt = packet_with_upload("scan.png", bytes);

    ChatParams cp = ChatParams::parse(pkt, 42);

    REQUIRE(cp.request.files.size() == 1);
    CHECK(cp.request.files[0].filename == "scan.png");
    CHECK(cp.request.files[0].file_key == "seg/a/abcdef.png");
    REQUIRE(cp.request.files[0].data.size() == bytes.size());
    CHECK(cp.request.files[0].data.str() == bytes);
}

TEST_CASE("the upload bytes are shared with the packet, not copied", "[chat][params]") {
    Packet pkt = packet_with_upload("scan.png", std::string(4096, '\x7f'));
    const char* attached = pkt.attachments()[0].data.data();

    ChatParams cp = ChatParams::parse(pkt, 42);

    REQUIRE(cp.request.files.size() == 1);
    CHECK(cp.request.files[0].data.data() == attached);
}

TEST_CASE("copying a file ref shares its bytes rather than duplicating them",
          "[chat][params]") {
    // The hop after parse: an agent builds an llm::FilePart from the const
    // request it was handed. That assignment must stay a handle copy.
    Packet pkt = packet_with_upload("scan.png", std::string(4096, '\x7f'));
    ChatParams cp = ChatParams::parse(pkt, 42);
    REQUIRE(cp.request.files.size() == 1);

    const mirobody::chat::AgentFile& f = cp.request.files[0];
    mirobody::llm::FilePart part;
    part.data = f.data;

    CHECK(part.data.data() == f.data.data());
    CHECK(part.data.data() == pkt.attachments()[0].data.data());
}

TEST_CASE("a file ref with no attachment keeps empty bytes", "[chat][params]") {
    // Reference-only: an upload from an earlier turn, carried by url/file_key
    // with nothing attached. The agent falls back to the URL.
    Packet pkt = packet_with_upload("scan.png", std::string(64, 'x'));
    ChatParams cp = ChatParams::parse(pkt, 42);
    REQUIRE(cp.request.files.size() == 1);

    Packet bare(kOpChat);
    {
        rapidjson::Document::AllocatorType& a = bare.allocator();
        rapidjson::Value& p = bare.params_mut();
        p.AddMember("question", rapidjson::Value("and this one?", a), a);
        rapidjson::Value f(rapidjson::kObjectType);
        f.AddMember("filename", rapidjson::Value("old.png", a), a);
        f.AddMember("url", rapidjson::Value("https://example.test/old.png", a), a);
        rapidjson::Value files(rapidjson::kArrayType);
        files.PushBack(f, a);
        p.AddMember("files", files, a);
    }

    ChatParams bp = ChatParams::parse(bare, 42);
    REQUIRE(bp.request.files.size() == 1);
    CHECK(bp.request.files[0].data.empty());
    CHECK(bp.request.files[0].data.size() == 0);
    CHECK(bp.request.files[0].url == "https://example.test/old.png");
    // An unset Blob still reads as an empty string rather than dangling.
    CHECK(bp.request.files[0].data.str().empty());
}

TEST_CASE("bytes are matched to their ref by filename", "[chat][params]") {
    // Two uploads in one turn: each ref must get its own bytes, not the first
    // attachment's.
    Packet pkt(kOpChat);
    rapidjson::Document::AllocatorType& a = pkt.allocator();
    rapidjson::Value& p = pkt.params_mut();
    p.AddMember("question", rapidjson::Value("compare these", a), a);

    rapidjson::Value files(rapidjson::kArrayType);
    for (const char* name : {"first.png", "second.png"}) {
        rapidjson::Value f(rapidjson::kObjectType);
        f.AddMember("filename", rapidjson::Value(name, a), a);
        files.PushBack(f, a);
    }
    p.AddMember("files", files, a);

    Attachment second;
    second.filename = "second.png";
    second.data     = Blob(std::string("SECOND"));
    pkt.add_attachment(std::move(second));

    Attachment first;
    first.filename = "first.png";
    first.data     = Blob(std::string("FIRST"));
    pkt.add_attachment(std::move(first));

    ChatParams cp = ChatParams::parse(pkt, 42);

    REQUIRE(cp.request.files.size() == 2);
    CHECK(cp.request.files[0].data.str() == "FIRST");
    CHECK(cp.request.files[1].data.str() == "SECOND");
}
