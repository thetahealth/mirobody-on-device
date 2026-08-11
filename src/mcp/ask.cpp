#include "mcp/ask.hpp"

#include <condition_variable>
#include <map>
#include <mutex>

namespace mirobody { namespace mcp {

namespace {

struct Slot {
    std::string answer;
    bool        settled = false;   // answered OR abandoned; both end the wait
};

std::mutex                        g_mutex;
std::condition_variable           g_cv;
std::map<std::string, Slot>       g_slots;

// The id the CURRENT thread is about to wait on. Set by expect(), consumed by
// wait(). A thread-local rather than a parameter because the two calls come from
// different layers -- the chat tier's filter and the tool handler -- that share a
// thread but deliberately share no signature.
thread_local std::string t_expected;

}   // namespace

AskBroker& AskBroker::instance() {
    static AskBroker broker;
    return broker;
}

void AskBroker::expect(const std::string& ask_id) {
    if (ask_id.empty()) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_slots[ask_id] = Slot();
    }
    t_expected = ask_id;
}

std::string AskBroker::wait(int timeout_seconds) {
    const std::string id = t_expected;
    t_expected.clear();          // one wait per expect; never inherit a stale id
    if (id.empty()) return std::string();

    std::unique_lock<std::mutex> lock(g_mutex);
    std::map<std::string, Slot>::iterator it = g_slots.find(id);
    if (it == g_slots.end()) return std::string();

    // Predicate form, so a cancel_all() that lands between expect() and wait()
    // is not lost and a spurious wake does not end the wait early.
    const bool settled = g_cv.wait_for(
        lock, std::chrono::seconds(timeout_seconds > 0 ? timeout_seconds : 1),
        [&id]() {
            std::map<std::string, Slot>::iterator s = g_slots.find(id);
            return s == g_slots.end() || s->second.settled;
        });

    std::string out;
    it = g_slots.find(id);
    if (settled && it != g_slots.end()) {
        out = it->second.answer;
    }
    if (it != g_slots.end()) {
        g_slots.erase(it);       // the slot dies with the wait, answered or not
    }
    return out;
}

bool AskBroker::answer(const std::string& ask_id, const std::string& answer_json) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::map<std::string, Slot>::iterator it = g_slots.find(ask_id);
        if (it == g_slots.end() || it->second.settled) {
            return false;        // nobody waiting, or already answered
        }
        it->second.answer  = answer_json;
        it->second.settled = true;
    }
    g_cv.notify_all();
    return true;
}

void AskBroker::cancel_all() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (std::map<std::string, Slot>::iterator it = g_slots.begin();
             it != g_slots.end(); ++it) {
            it->second.settled = true;   // answer stays empty => the waiter sees none
        }
    }
    g_cv.notify_all();
}

}}
