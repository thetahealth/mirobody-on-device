#pragma once

// Ingest pipeline: raw health readings -> projected health_facts rows.
//
// This is the SYNCHRONOUS TRANSFORM half of health-data ingestion. It takes the
// readings flattened out of an upload / FHIR Observation and turns them into the
// facts persisted in health_facts (see res/sql/<dialect>/1_data.sql): validating
// them, normalizing units, and downsampling high-frequency device streams into
// per-bucket aggregates.
//
// The shape is deliberately the chat::EventPipeline model
// (src/chat/event/filter/filter.hpp): an ordered chain of push-model stages,
// each free to drop / rewrite / merge the facts flowing through, terminating at
// a caller-supplied sink. Two differences from EventPipeline, both driven by
// rollup:
//   - stages carry a finish() to flush buffered state -- a rollup bucket only
//     closes when the batch ends; and
//   - the pipeline is monomorphic on Fact: a raw sample enters as a degenerate
//     point fact (effective_start == effective_end -- one instant), and rollup
//     merges a run of same-bucket point facts into one aggregate fact. So a raw
//     sample is just a fact with count 1, and rollup is fact-in/fact-out.
//
// What this pipeline is NOT: it does not archive raw samples. Keeping every
// device sample (the parquet cold store) is a SEPARATE, deferred job the ingest
// orchestrator runs alongside this transform -- folding object-store IO and its
// retry/concurrency model into a pure transform is the conflation we explicitly
// avoid. This pipeline's single output is facts.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mirobody { namespace health { namespace timeseries {

// Source class of a fact; matches health_facts.origin (1_data.sql).
enum Origin {
    ORIGIN_DEVICE   = 1,
    ORIGIN_EHR      = 2,
    ORIGIN_DOCUMENT = 3,
    ORIGIN_MANUAL   = 4
};

// Decode recipe for a health_ingest_staging.payload -- the shape + codec the
// worker must handle to turn the bytes into readings. APPEND-ONLY: assign the
// next integer to each new recipe, never renumber. A worker advertises the
// highest value its build can decode and claims only rows with `format <= that
// max`, so a too-new format waits for an upgraded worker instead of being
// claimed and failing -- rolling-upgrade safe, and the reason format is an
// ordered int rather than a free string.
enum IngestFormat {
    FMT_FHIR_BUNDLE      = 1,  // FHIR Bundle JSON, uncompressed
    FMT_RAW_SAMPLES      = 2,  // raw sample array JSON, uncompressed
    FMT_RAW_SAMPLES_ZSTD = 3   // raw sample array JSON, zstd-compressed
};

// One observation flowing through the pipeline == one health_facts row on the
// way out. user_id is deliberately NOT here: the pipeline is a per-user but
// user-agnostic transform; the terminal sink closes over the user id when it
// writes the row. Likewise `id`-less -- health_facts has no surrogate key.
struct Fact {
    std::int64_t indicator_id   = 0;    // packed (system,code) fhir_id (health_indicators.id)
    bool         has_num        = false;// true => value_num holds a numeric reading
    double       value_num      = 0.0;  // numeric value (the mean, once rolled up)
    std::string  value_text;            // qualitative / coded result (when !has_num)
    std::string  unit;                  // unit; canonical after NormalizeUnitStage
    std::int64_t effective_start = 0;   // unix ms; the sample instant, or bucket start
    std::int64_t effective_end   = 0;   // unix ms; == effective_start for an instant
    std::uint8_t origin         = 0;    // Origin
    std::string  source_id;             // fhir_resources.resource_id back-pointer (always an Observation)
};

// A pipeline stage. Mirrors chat::EventFilter, plus finish() for buffered state.
class Stage {
public:
    // Forward one (possibly rewritten) fact downstream. Returns false when the
    // consumer is gone / the batch is aborted; a stage must propagate that.
    typedef std::function<bool(const Fact&)> Sink;

    virtual ~Stage() = default;

    // Process one fact, emitting 0..N facts through `out`. Passthrough is just
    // `return out(f);`. Return false to abort the batch.
    virtual bool feed(const Fact& f, const Sink& out) = 0;

    // Flush buffered facts (e.g. open rollup buckets) at end of batch, emitting
    // through `out` like feed(). Default: nothing buffered.
    virtual bool finish(const Sink& /*out*/) { return true; }
};

// An ordered chain of stages terminating at a final sink. feed() threads one
// fact through every stage (facts a stage emits flow into the rest of the chain,
// so stages compose); finish() flushes stages in order, each stage's flushed
// facts still passing through the stages after it before reaching `final`. Built
// once per batch (stages hold per-batch state). Header-only like EventPipeline.
class IngestPipeline {
public:
    void add(std::unique_ptr<Stage> stage) { stages_.push_back(std::move(stage)); }

    bool feed(const Fact& f, const Stage::Sink& final) const {
        return feed_from(0, f, final);
    }

    // Call once, after the last feed(), to close buffered aggregates.
    bool finish(const Stage::Sink& final) const {
        return finish_from(0, final);
    }

private:
    bool feed_from(std::size_t i, const Fact& f, const Stage::Sink& final) const {
        if (i == stages_.size()) return final(f);
        return stages_[i]->feed(f, [this, i, &final](const Fact& out) {
            return feed_from(i + 1, out, final);
        });
    }

    bool finish_from(std::size_t i, const Stage::Sink& final) const {
        if (i == stages_.size()) return true;
        // Stage i flushes into the FEED of the stages after it, so its aggregates
        // still get normalized/rolled-up downstream and reach `final`; then those
        // later stages finish in turn.
        bool ok = stages_[i]->finish([this, i, &final](const Fact& out) {
            return feed_from(i + 1, out, final);
        });
        return ok && finish_from(i + 1, final);
    }

    std::vector<std::unique_ptr<Stage> > stages_;
};

//------------------------------------------------------------------------------
// Stages
//------------------------------------------------------------------------------

// Drop facts that cannot project: no resolved indicator (indicator_id == 0),
// non-positive time, or a non-finite numeric value. The dropped count is logged
// at finish() -- a miss is made visible and fixable, never silently swallowed
// (same philosophy as the indicator resolver: an empty/dropped result beats a
// confident-wrong one).
class ValidateStage : public Stage {
public:
    bool feed(const Fact& f, const Sink& out) override;
    bool finish(const Sink& out) override;
private:
    std::int64_t dropped_ = 0;
    std::int64_t passed_  = 0;
};

// Convert (value_num, unit) to the indicator's canonical unit via fhir/units.
// Qualitative facts (has_num == false) pass through untouched.
//
// STATUS: draft passthrough. Wiring the fhir::units normalization (and each
// indicator's canonical unit) is the follow-up; kept as an explicit stage so the
// seam exists and the pipeline order is right.
class NormalizeUnitStage : public Stage {
public:
    bool feed(const Fact& f, const Sink& out) override;
};

// Downsample high-frequency numeric streams into per-bucket aggregate facts.
// `bucket_ms(indicator_id)` returns the bucket width in unix ms, or 0 to pass a
// fact through unchanged -- so discrete labs / vitals stay 1:1 (bucket_ms == 0)
// while a 1 Hz heart-rate stream (bucket_ms == 86'400'000 for daily) collapses to
// one fact per bucket. Buckets align to the unix epoch (idx = start / width), so
// daily buckets are UTC days; timezone-aware bucketing is a later policy concern.
//
// Buffers numeric point facts per (indicator_id, bucket) and emits one fact per
// bucket at finish(): value_num = mean, effective_start/end = bucket bounds.
// (value_min / value_max / sample_count would ride here too if health_facts
// gains those columns.) Qualitative facts never aggregate -- they pass through.
class RollupStage : public Stage {
public:
    typedef std::function<std::int64_t(std::int64_t indicator_id)> BucketMs;

    explicit RollupStage(BucketMs bucket_ms) : bucket_ms_(std::move(bucket_ms)) {}

    bool feed(const Fact& f, const Sink& out) override;
    bool finish(const Sink& out) override;

private:
    struct Acc {
        double       sum   = 0.0;
        std::int64_t count = 0;
        std::int64_t start = 0;   // bucket lower bound (inclusive), unix ms
        std::int64_t end   = 0;   // bucket upper bound (inclusive), unix ms
        std::string  unit;
        std::uint8_t origin = 0;
        std::string  source_id;   // source of the first contributing sample; when
                                  // a bucket spans multiple sources the orchestrator
                                  // repoints it to the summary Observation (deferred).
    };
    // Keyed (indicator_id, bucket index); std::map keeps finish() output ordered.
    std::map<std::pair<std::int64_t, std::int64_t>, Acc> buckets_;
    BucketMs bucket_ms_;
};

//------------------------------------------------------------------------------

// Assemble the standard ingest pipeline: Validate -> NormalizeUnit -> Rollup.
// `bucket_ms` is the per-indicator bucketing policy (0 => no rollup). The caller
// supplies the terminal sink -- it writes each Fact to health_facts, closing over
// user_id -- and, SEPARATELY, archives the raw samples to the parquet cold store.
IngestPipeline make_ingest_pipeline(RollupStage::BucketMs bucket_ms);

}}}  // namespace mirobody::health::timeseries
