#include "chat/event/filter/filter.hpp"

#include "chat/event/filter/chart.hpp"

namespace mirobody { namespace chat {

EventPipeline make_event_pipeline() {
    EventPipeline p;
    p.add(std::unique_ptr<EventFilter>(new ChartFilter()));
    // Register additional filters here, e.g.:
    //   p.add(std::unique_ptr<EventFilter>(new CitationFilter()));
    return p;
}

}}
