#pragma once

// StreamWriter: the thread-safe async streaming channel, split out of
// server/router.hpp and made header-only. response.hpp includes this, so
// #include "server/router.hpp" (or response.hpp) still pulls it in.
//
// The methods are tiny and lock a single mutex, so they live inline here rather
// than in a .cpp. The only libwebsockets dependency is lws_cancel_service (the
// wakeup that nudges the single-threaded service loop); it is forward-declared
// below so this header stays light -- includers do not pull in <libwebsockets.h>.

#include "compat/cxx11.hpp"

#include <chrono>
#include <mutex>
#include <string>
#include <utility>

struct lws_context;

// The one libwebsockets entry point StreamWriter needs. Declared (not included)
// to keep <libwebsockets.h> out of every translation unit that streams; the
// signature matches libwebsockets.h verbatim, so a TU that includes both (e.g.
// router.cpp) sees a compatible redeclaration.
extern "C" void lws_cancel_service(struct lws_context *context);

namespace mirobody { namespace server {

//------------------------------------------------------------------------------
// Asynchronous streaming channel
//------------------------------------------------------------------------------

// A thread-safe sink that bridges a worker thread to the single-threaded lws
// service loop, so a handler can stream a response whose bytes are produced off
// the service thread (e.g. proxying an upstream streaming HTTP response). The
// worker calls begin() once with the status and Content-Type, then write() per
// chunk, then finish(); each call wakes the service thread via lws_cancel_service
// so the router can relay buffered bytes over chunked transfer-encoding. The
// worker should poll cancelled() and stop early once the client disconnects.
// Obtain one from Response::begin_stream(); never construct it directly.
class StreamWriter {
public:
    explicit StreamWriter(lws_context* ctx) : ctx_(ctx) {}

    StreamWriter(const StreamWriter&)            = delete;
    StreamWriter& operator=(const StreamWriter&) = delete;

    //-- Producer side (any thread). -------------------------------------------

    void begin(long status, std::string content_type) {
        {
            std::lock_guard<std::mutex> lk(m_);
            status_ = status;
            content_type_ = std::move(content_type);
            headers_ready_ = true;
            last_activity_ = std::chrono::steady_clock::now();
        }
        wake();
    }

    void write(const std::string& bytes) {
        {
            std::lock_guard<std::mutex> lk(m_);
            buf_.append(bytes.data(), bytes.size());
            last_activity_ = std::chrono::steady_clock::now();
        }
        wake();
    }

    // Append `bytes` unless the stream is already finished; false when it was.
    // The check and the append happen under one lock, so a second producer (the
    // keepalive pump) cannot slip bytes in behind the terminal chunk however it
    // races the thread that ends the stream.
    bool write_unless_finished(const std::string& bytes) {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (finished_) return false;
            buf_.append(bytes.data(), bytes.size());
            last_activity_ = std::chrono::steady_clock::now();
        }
        wake();
        return true;
    }

    void finish() {                        // end the stream cleanly
        {
            std::lock_guard<std::mutex> lk(m_);
            finished_ = true;
        }
        wake();
    }

    // Append the final bytes and end the stream in one atomic step, so take()
    // reports end-of-stream together with the last data. This lets the router
    // mark the closing write LWS_WRITE_HTTP_FINAL -- the only reliable way to
    // emit the terminating chunk of a chunked body (a separate zero-length FINAL
    // write is a no-op in lws, leaving the client with a truncated response).
    // Prefer this over write()+finish() whenever the producer knows its last
    // bytes, which otherwise race: take() can drain them before finish() lands.
    void finish(const std::string& final_bytes) {
        {
            std::lock_guard<std::mutex> lk(m_);
            buf_.append(final_bytes.data(), final_bytes.size());
            finished_ = true;
        }
        wake();
    }

    void fail() {                          // end it as broken (abort the connection)
        {
            std::lock_guard<std::mutex> lk(m_);
            finished_ = true;
            failed_ = true;
        }
        wake();
    }

    bool cancelled() const {
        std::lock_guard<std::mutex> lk(m_);
        return cancelled_;
    }

    bool headers_ready() const {
        std::lock_guard<std::mutex> lk(m_);
        return headers_ready_;
    }

    // True once the stream has been ended (by finish() or fail()).
    bool is_finished() const {
        std::lock_guard<std::mutex> lk(m_);
        return finished_;
    }

    // Milliseconds since the last bytes were queued (or since begin()). What a
    // keepalive producer consults so it stays quiet while real data is flowing.
    long long idle_ms() const {
        std::lock_guard<std::mutex> lk(m_);
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - last_activity_).count());
    }

    //-- Service-thread side, used by the router's writeable callback. ---------

    long status() const {
        std::lock_guard<std::mutex> lk(m_);
        return status_;
    }

    std::string content_type() const {
        std::lock_guard<std::mutex> lk(m_);
        return content_type_;
    }

    // Move any buffered bytes into `out` and report whether the stream is
    // finished. Reading the buffer and the finished flag under one lock is what
    // makes the terminate decision race-free: a chunk the producer adds
    // concurrently is either taken here or leaves finished() observably false
    // for the next call.
    bool take(std::string& out) {
        std::lock_guard<std::mutex> lk(m_);
        if (!buf_.empty()) {
            out.append(buf_);
            buf_.clear();
        }
        return finished_;
    }

    bool failed() const {                  // finished via fail() rather than finish()
        std::lock_guard<std::mutex> lk(m_);
        return failed_;
    }

    void cancel() {                        // mark the client gone
        std::lock_guard<std::mutex> lk(m_);
        cancelled_ = true;
    }

private:
    // Nudge the single-threaded service loop to relay buffered bytes / headers.
    void wake() { if (ctx_) lws_cancel_service(ctx_); }

    lws_context*       ctx_;
    mutable std::mutex m_;
    std::string        buf_;
    // When bytes were last queued -- steady_clock, so a wall-clock adjustment
    // cannot make a stream look idle (or busy) when it isn't.
    std::chrono::steady_clock::time_point last_activity_ = std::chrono::steady_clock::now();
    long               status_        = 200;
    std::string        content_type_  = "application/octet-stream";
    bool               headers_ready_ = false;
    bool               finished_      = false;
    bool               failed_        = false;
    bool               cancelled_     = false;
};

}
}
