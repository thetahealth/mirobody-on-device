#pragma once

// Event filters (chat interface tier).
//
// A filter is a stream transform over chat::Events: it watches the event stream
// and may suppress, rewrite, or collapse events into new ones -- e.g. ChartFilter
// recognizes a render_chart tool call (the queryTitle/queryArguments/queryDetail
// triple) and replaces it with a single ChartEvent. Filters run entirely in the
// chat tier on chat::Event, so adding one never touches the lower llm layer.
//
// To add a filter: write an EventFilter subclass (see event/filter/) and add one
// line to make_event_pipeline(). That's the whole recipe -- no llm changes, no
// transport changes.
//
// The dispatcher builds a fresh pipeline per turn (filters hold per-stream state)
// and feeds it the chat events it translates from the agent's llm stream; the
// pipeline's output is what the Responder sends.

#include "chat/event/event.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace mirobody { namespace chat {

class EventFilter {
public:
    // Downstream sink: forward one (possibly rewritten) event. Returns false when
    // the client is gone, which aborts the turn.
    typedef std::function<bool(const Event&)> Sink;

    virtual ~EventFilter() = default;

    // Process one event, emitting zero or more events through `out`. Return false
    // to abort (propagate a false from `out`). A pass-through filter is just
    // `return out(e);`.
    virtual bool feed(const Event& e, const Sink& out) = 0;
};

// An ordered chain of filters terminating at a final sink. feed() pushes one
// event through every filter in turn; events a filter emits flow into the rest of
// the chain, so filters compose. Built fresh per turn.
class EventPipeline {
public:
    void add(std::unique_ptr<EventFilter> filter) {
        filters_.push_back(std::move(filter));
    }

    bool feed(const Event& e, const EventFilter::Sink& final) const {
        return feed_from(0, e, final);
    }

private:
    bool feed_from(std::size_t i, const Event& e, const EventFilter::Sink& final) const {
        if (i == filters_.size()) return final(e);
        return filters_[i]->feed(e, [this, i, &final](const Event& out) {
            return feed_from(i + 1, out, final);
        });
    }

    std::vector<std::unique_ptr<EventFilter> > filters_;
};

// Build the chat event-filter pipeline. THE one place to register a filter: add
// a line. Called once per turn by the dispatcher.
EventPipeline make_event_pipeline();

}}
