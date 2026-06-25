// Minimal polyfills so the C++17 code we already have can compile against
// a C++11 toolchain. Drop-in for what the project actually uses; not full
// std:: API coverage.

#pragma once

#include <cstddef>
#include <cstring>
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

}
