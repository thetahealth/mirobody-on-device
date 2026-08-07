// Minimal polyfills so the C++17 code we already have can compile against
// a C++11 toolchain. Drop-in for what the project actually uses; not full
// std:: API coverage.

#pragma once

#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

namespace mirobody {

//------------------------------------------------------------------------------
// optional + nullopt
//------------------------------------------------------------------------------

struct nullopt_t {};
const nullopt_t nullopt = {};

template <typename T>
class optional {
public:
    optional() : has_(false) {}
    optional(nullopt_t) : has_(false) {}
    optional(const T& v) : has_(true) { new (storage()) T(v); }
    optional(T&& v) : has_(true) { new (storage()) T(std::move(v)); }

    optional(const optional& o) : has_(o.has_) {
        if (has_) new (storage()) T(*o);
    }
    optional(optional&& o) : has_(o.has_) {
        if (has_) new (storage()) T(std::move(*o));
    }

    optional& operator=(const optional& o) {
        if (this == &o) return *this;
        reset();
        if (o.has_) { new (storage()) T(*o); has_ = true; }
        return *this;
    }
    optional& operator=(optional&& o) {
        if (this == &o) return *this;
        reset();
        if (o.has_) { new (storage()) T(std::move(*o)); has_ = true; }
        return *this;
    }
    optional& operator=(nullopt_t) { reset(); return *this; }
    optional& operator=(const T& v) {
        reset(); new (storage()) T(v); has_ = true; return *this;
    }
    optional& operator=(T&& v) {
        reset(); new (storage()) T(std::move(v)); has_ = true; return *this;
    }

    ~optional() { reset(); }

    void reset() { if (has_) { ptr()->~T(); has_ = false; } }

    bool has_value() const { return has_; }
    explicit operator bool() const { return has_; }

    T&        operator*()        { return *ptr(); }
    const T&  operator*()  const { return *ptr(); }
    T*        operator->()       { return  ptr(); }
    const T*  operator->() const { return  ptr(); }

    T&        value()       { return *ptr(); }
    const T&  value() const { return *ptr(); }

    template <typename U>
    T value_or(U&& fallback) const {
        return has_ ? *ptr() : T(std::forward<U>(fallback));
    }

private:
    T*       ptr()       { return static_cast<T*>(storage()); }
    const T* ptr() const { return static_cast<const T*>(storage()); }
    void*    storage()       { return &buf_; }
    const void* storage() const { return &buf_; }

    typename std::aligned_storage<sizeof(T), alignof(T)>::type buf_;
    bool has_;
};

//------------------------------------------------------------------------------
// Blob -- shared, immutable bytes
//------------------------------------------------------------------------------

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

}
