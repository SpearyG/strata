# Implementation contract

Strata is a bounded, key-partitioned telemetry aggregation library, CLI and benchmark.
No networking, distributed guarantees, or database durability claims. C++26 mode,
guarded pack indexing in a compile-time pipeline, standard library only.

Shared core headers: event {sequence uint64, series uint32, value double,
timestamp_ns uint64}; config {workers size_t, queue_capacity size_t, batch_size
size_t, series_count uint32, measure_latency bool}. Error is {errc code,
std::string message, uint64 offset}. See headers for enums. Accepted values finite
and absolute value <= 1e100. Logical identity is series, not sequence.

Runtime API to implement in include/strata/runtime/engine.hpp:
- engine::create(config) -> expected<unique_ptr<engine>,error>
- submit(event, stop_token={}) -> expected<void,error> (blocking backpressure)
- try_submit(event) -> expected<void,error>
- close() noexcept: concurrent-safe, idempotent stop admission + drain + join
- cancel() noexcept: concurrent-safe, idempotent stop admission + discard queued + join
- snapshot() const -> engine_snapshot, only after close/cancel; otherwise error
  via expected<engine_snapshot,error>. No concurrent worker data reads.
- destructor drains. Noncopyable/nonmovable. Join serialization must avoid
  deadlock with active submitters. Acceptance linearizes under channel mutex.
- engine_snapshot fields: uint64 accepted, processed, rejected, discarded;
  size_t queue_high_watermark; metrics::latency_histogram latency;
  vector<series_summary> series. series_summary {uint32 series; uint64 count;
  double mean, m2, minimum, maximum}. Include all keys (empty count = 0).
- count invariants after join: accepted = processed + discarded.
- configuration() const -> const config&.
- Worker-owned shards, preallocated ring and aggregates, batch dequeue. Metrics
  merged after join, no global hot-path atomic increment for processed.

Storage API (independent of engine) in include/strata/storage/journal.hpp:
- journal_writer::open(path) -> expected<unique_ptr<journal_writer>,error>;
  creates/truncates; append(span<const event>) -> expected<void,error>;
  flush() -> expected<void,error>. Move/copy disabled; destructor closes.
- replay(path, callback) -> expected<replay_result,error>; callback is
  std::function<expected<void,error>(span<const event>)> synchronous;
  replay_result {uint64 events, frames; bool truncated_tail}.
- Explicit versioned little endian frame format, bounded allocation on hostile
  input, CRC32C; complete corruption rejected. Incomplete final frame tolerated
  with truncated_tail=true. No fsync guarantee. Append is single owner.
- public encode_frame/decode_frame fuzz-friendly span API, documented limits.

CLI executable strata: --workers --producers --events --queue-capacity --batch-size
--series --seed --burst --work [integer CPU rounds] --no-latency --journal PATH
--replay PATH. Journal generation/replay separate from concurrent measured run.
CLI workload uses runtime API. Config validation + from_chars; JSON metrics.
Benchmark executable strata_bench: repeats >= 5 default, warmup, compare
batch_size=1 baseline and chosen batch size; observable exact accepted/processed
counts, configurable producers/workers/events/series/capacity/seed/work/burst.
Work simulation in producer is acceptable only if documented. Histograms must
state end-to-end admission-attempt-to-processing latency and bucket bounds.

Agents own distinct files; root owns core headers, pipeline, CMake, runtime tests,
README/docs/CI. Runtime agent owns concurrency, runtime, metrics headers/sources.
Storage agent owns storage headers/source and tests/test_storage.cpp.
Workload agent owns tools/, benchmarks/, examples/ and workload headers/sources.
Do not commit: root stages coherent changes once tests/builds pass. Tests use
standalone main returning nonzero and throw on failed checks (no assert/NDEBUG).
