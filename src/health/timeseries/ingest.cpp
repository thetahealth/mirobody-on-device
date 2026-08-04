#include "health/timeseries/ingest.hpp"

#include "platform/log.hpp"

#include <cmath>

namespace mirobody { namespace health { namespace timeseries {

//------------------------------------------------------------------------------
// ValidateStage
//------------------------------------------------------------------------------

bool ValidateStage::feed(const Fact& f, const Sink& out) {
    const bool ok = f.indicator_id != 0 &&
                    f.effective_start > 0 &&
                    (!f.has_num || std::isfinite(f.value_num));
    if (!ok) {
        ++dropped_;
        return true;   // drop this fact, keep processing the batch
    }
    ++passed_;
    return out(f);
}

bool ValidateStage::finish(const Sink& /*out*/) {
    if (dropped_ != 0) {
        platform::log_warn("health ingest: dropped %lld unprojectable reading(s), kept %lld",
                           static_cast<long long>(dropped_),
                           static_cast<long long>(passed_));
    }
    return true;
}

//------------------------------------------------------------------------------
// NormalizeUnitStage
//------------------------------------------------------------------------------

bool NormalizeUnitStage::feed(const Fact& f, const Sink& out) {
    // TODO: convert (f.value_num, f.unit) to the indicator's canonical unit via
    // fhir::units and rewrite value_num/unit. Qualitative facts stay untouched.
    // Draft: passthrough so the stage seam exists and the order is fixed.
    return out(f);
}

//------------------------------------------------------------------------------
// RollupStage
//------------------------------------------------------------------------------

bool RollupStage::feed(const Fact& f, const Sink& out) {
    const std::int64_t width = f.has_num ? bucket_ms_(f.indicator_id) : 0;

    // Discrete / qualitative -> passthrough 1:1 (an instant: end == start upstream).
    if (width <= 0)
        return out(f);

    const std::int64_t idx   = f.effective_start / width;   // epoch-aligned bucket
    const std::int64_t start = idx * width;
    Acc& a = buckets_[std::make_pair(f.indicator_id, idx)];
    if (a.count == 0) {                 // first sample in this bucket
        a.start       = start;
        a.end         = start + width - 1;
        a.unit        = f.unit;
        a.origin      = f.origin;
        a.source_id   = f.source_id;
    }
    a.sum += f.value_num;
    ++a.count;
    return true;                        // buffered; nothing emitted until finish()
}

bool RollupStage::finish(const Sink& out) {
    // std::map iterates by (indicator_id, bucket) ascending -> deterministic order.
    for (std::map<std::pair<std::int64_t, std::int64_t>, Acc>::const_iterator it =
             buckets_.begin(); it != buckets_.end(); ++it) {
        const Acc& a = it->second;
        Fact f;
        f.indicator_id    = it->first.first;
        f.has_num         = true;
        f.value_num       = a.sum / static_cast<double>(a.count);   // mean
        f.unit            = a.unit;
        f.effective_start = a.start;
        f.effective_end   = a.end;
        f.origin          = a.origin;
        f.source_id       = a.source_id;
        if (!out(f)) return false;
    }
    buckets_.clear();
    return true;
}

//------------------------------------------------------------------------------

IngestPipeline make_ingest_pipeline(RollupStage::BucketMs bucket_ms) {
    IngestPipeline p;
    p.add(std::unique_ptr<Stage>(new ValidateStage()));
    p.add(std::unique_ptr<Stage>(new NormalizeUnitStage()));
    p.add(std::unique_ptr<Stage>(new RollupStage(std::move(bucket_ms))));
    return p;
}

}}}  // namespace mirobody::health::timeseries
