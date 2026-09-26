#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace mirobody {

// A block of bytes with shared ownership: copying a Blob copies a pointer, not
// the bytes. It exists for payloads that travel a long chain by value -- an
// uploaded file goes transport -> Packet attachment -> AgentFile -> llm::FilePart
// -> the provider request -- where each hop must own what it holds (any hop may
// outlive the one before it) but none may modify it. Holding std::string at each
// hop meant one full copy per hop that took its input by const reference, which
// is every hop: you cannot move out of a const source. Sharing an immutable
// buffer removes the copies without making any hop's ownership weaker.
//
// The payload is std::string rather than a byte vector because every producer
// and consumer of these bytes already speaks std::string -- the HTTP body, the
// multipart part, storage::put_object, the base64 encoders -- so the bytes move
// in and out with no conversion. It is a byte buffer, not text: embedded NULs
// are fine, so read it via str() / data() / size(), never c_str().
//
// A default-constructed Blob holds no buffer and reads as empty, so empty() is
// the only check a caller needs before using str(). Deliberately NOT implicitly
// convertible to std::string: the conversion would silently copy the bytes into
// any by-value parameter, which is the very cost this type removes -- callers
// name str() so the read is visible.
class Blob {
public:
    Blob() {}
    explicit Blob(std::string bytes)
        : buf_(std::make_shared<const std::string>(std::move(bytes))) {}

    bool        empty() const { return !buf_ || buf_->empty(); }
    std::size_t size()  const { return buf_ ? buf_->size() : 0; }

    // The bytes. Safe on an unset Blob: it reads as an empty string rather than
    // dangling, so callers need no null check.
    const std::string& str() const {
        static const std::string kEmpty;
        return buf_ ? *buf_ : kEmpty;
    }
    const char* data() const { return str().data(); }

private:
    std::shared_ptr<const std::string> buf_;
};

}  // namespace mirobody
