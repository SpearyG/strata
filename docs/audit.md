# Staff-level adversarial audit

Completed 2026-11-02 against private repository `SpearyG/strata`, baseline
`848d59e83de36597dd02220eaeef6aea41496a15`. Work is proposed on
`audit/staff-level-review`; this audit does not merge it into `main`.

## Scope and interpretation

All 36 tracked baseline files and all 11 baseline commits were inspected,
including public headers, implementations, tests, examples, build/install rules,
workflow, README and architecture/format/performance documents. Added and changed
files were reviewed again. Separate reviewers checked core concurrency,
storage/numerics, and benchmark/CI behavior, followed by independent final review.

Severity totals: **0 critical, 0 high, 9 medium, 6 low**. These 15 findings include
confirmed defects and lower-severity API, measurement and assurance gaps; they are
not 15 demonstrated production crashes. No data race or engine shutdown deadlock
was reproduced. Passing tests and sanitizers are evidence, not a proof of universal
correctness or production qualification.

## Severity-ranked findings

| ID | Severity | Baseline problem and evidence | Disposition |
| --- | --- | --- | --- |
| M1 | Medium | A legal move may retain resources in its source. `pop_batch` left such objects in logically empty ring slots; weak ownership stayed alive after pop, consumer release and cancel of an empty queue. | Reset popped nontrivially-copyable slots; regressions cover retaining moves and trivial destructors with custom assignment. Trivial telemetry has no extra reset. |
| M2 | Medium | Percentile rank `ceil(long double(count) * fraction)` can round down before count overflow. For a valid `UINT64_MAX` merged count and binary64 `.95`, the correct rank is `17524406870024073216`; the old result was one smaller and selected 1ns instead of 1024ns. | Exact bounded two-word unsigned arithmetic, no nonstandard integer extension in the library. Bucket, subnormal, nextafter, merge and huge-count regressions. |
| M3 | Medium | `workers * capacity` wrapped on 32-bit `size_t`: `256 * 16777216` became zero and passed the combined-capacity limit in runtime and consteval validation. | Division-based guards after nonzero checks. Boundary tests and a 32-bit header compilation probe; no complete 32-bit runtime/toolchain validation is claimed. |
| M4 | Medium | GCC 15 rejected `pipeline<>` on the pack-index path while Clang and tuple fallback accepted it. The constrained member alias still instantiated empty-pack indexing too early. | Defer indexing through a constrained helper; empty pipeline and stage type parity checked in default/fallback builds. |
| M5 | Medium | Clang analyzer findings normally exit zero, including with ordinary `-Werror`; the CI analysis step could be green with a null-dereference finding. | `-Xanalyzer -analyzer-werror`; a deliberately bad temporary source proves the shared runner fails. Analysis covers library, CLI and benchmark translation units. |
| M6 | Medium | Negative CLI CTests used `WILL_FAIL`, accepting an unintended normal exit1, including sanitizer failures, instead of the intended validation exit2. | Python wrapper requires exit2, valid JSON and no sanitizer signature for nine invalid-input cases. |
| M7 | Medium | Installed instrumented static library exported no sanitizer runtime link requirement. A normal `find_package(strata)` consumer failed to link ASan/UBSan symbols. | Export runtime link options from the archive; independent installed-consumer configure/build/run is now a CTest, including sanitizer configurations. |
| M8 | Medium | Both executables returned success when stdout was `/dev/full`, silently losing JSON results. | Explicit flush/state check and error exit1; CLI and benchmark regression checks. |
| M9 | Medium | Replay SIGINT returned only an error string, contrary to the documented partial accounting guarantee. A late signal before monitor polling could also be missed at replay completion. | Cancel/join/snapshot and report accepted/processed/discarded, complete-frame count, reason and completion status. Unknown tail status is null; other replay errors remain failures. FIFO-ready signal test avoids a pre-signal sleep. |
| L1 | Low | By-value generic sends consumed a moved owning argument even when full/closed/cancelled. This followed the signature, but made retry misuse easy and was not an explicit ownership contract. | Rvalue-reference admission moves only on success; copyable lvalue overload retained; owning rejection tests. |
| L2 | Low | Source checksum omitted series and timestamp; 1-series and 1024-series runs could have identical checksums despite different routing. The original benchmark also did not consume all aggregate fields under optional LTO. | Hash all four event fields and emit a post-timing aggregate digest. The audit harness additionally validates counts, finite moments, extrema and complete unique series IDs against a reference. |
| L3 | Low | Checkout used a mutable major tag, jobs lacked explicit deadlines, example CTest lacked a deadline, and empty-test discovery was not rejected. | Pin checkout v4.3.1 by full SHA; 20-minute jobs, bounded tests, explicit testing and `--no-tests=error`. No build cache is used. Toolchain major versions and sanitizer runtime packages remain explicit. |
| L4 | Low | The delivered 5M-event observation existed only in retained local JSONL, not repository documentation. Earlier before/after sessions were sequential. | Archive original and rerun evidence, commands, hashes and controlled AB/BA measurements in the performance record. Historical throughput is not a guarantee or a claim of exact reproduction. |
| L5 | Low | Generic payload cleanup runs under the channel mutex; reentrant payload operations can deadlock. The type concept cannot establish lock-order safety. | Document non-reentrancy and external-lock preconditions. This remains a caller obligation; no arbitrary callback-safe queue rewrite was attempted. |
| L6 | Low | Real `perf` samples identified shared stop-token ownership increments/decrements on each ready-queue submission. | Borrow token references for the synchronous engine/channel calls; blocking standard-library waits take their own copy. Retained because the controlled latency-disabled 8-producer case shows repeated workload-specific median improvement; default/hot-key differences remain noisy. |

## Concurrency invariants and reviewed nonbugs

- Queue mutex owns slots, head/tail, size, lifecycle and accounting. Insertion is
  the admission linearization point. `0 <= size <= capacity`; index advancement
  wraps before exceeding capacity. FIFO is insertion order within a shard.
- Consumer and producer waits inspect predicates under that mutex. Close/cancel
  changes state under it, then wakes both sides. Spurious wakeups recheck state.
  Batch dequeue notifies all producers because multiple slots may become free.
  No fairness or bounded scheduler delay is promised.
- Cancel discards queued objects, not a batch already moved into consumer-owned
  memory. A latch-held batch regression proves the accounting boundary. After
  producers finish and workers join, `accepted = processed + discarded`.
- Engine shutdown releases each queue mutex before taking the join mutex.
  Snapshot takes the join mutex, verifies completed join, then reads counters.
  There is no retained inverse queue/join lock ordering. Concurrent cancel may
  escalate drain; close never reopens a cancelled queue.
- Each aggregate/histogram has exactly one worker writer. Configuration is
  immutable, shards have stable heap addresses, and join publishes worker state.
  Invalid-input diagnostics use a relaxed atomic without publishing event data.
- Token references are not stored. A default or `get_token()` temporary lives
  through the synchronous call; the blocking condition-variable wait owns its
  required token copy. The caller must not concurrently mutate the same token
  object, just as with other borrowed arguments.
- Owners must finish caller-owned producer calls before destroying the engine.
  Concurrent destruction is invalid. Snapshot is post-shutdown, not a live view;
  rejected attempts can still increase until producers quiesce.

No rewrite of bounded mutex queues, sharding, worker ownership, notification
policy or cancellation semantics was justified. Hot-key imbalance, thundering
herd and lack of work stealing remain explicit tradeoffs.

## Lifetime, failure injection and test depth

The retained suites include capacity-one 4P/4C exact transfer, wraparound, full and
pre/racing-cancel ownership, concurrent close/cancel, owned batches, a 90% hot-key
one-producer/many-shard workload, and 36 seeded runtime lifecycle orderings after
every producer has completed a real admission. Existing 32 seeded stress
topologies remain active. No fixed sleep establishes a race-test ordering.

`startup_audit` discovers and faults 55 direct scalar/overaligned allocation sites
reached by project objects on the main thread in the tested Linux LP64 builds.
GNU-compatible target-only linker wrapping forwards to the original allocator,
preserving ASan/TSan interceptors. Failures include partially started worker pools;
every case unwinds and returns to the persistent runtime thread baseline. This
does not intercept allocations internal to shared libraries, arbitrary `malloc`,
or every possible OS failure. A separate manual 65-case probe (56 allocation
scenarios including a control, nine pthread-start scenarios including a control)
also completed without blocked workers or leaked threads.

An initial new test incorrectly equated join completion with immediate `/proc`
task removal. It failed once in 20 trials; one observation changed from four tasks
to one after a single 33,710ns poll. The test now yields to observed quiescence
with a two-second deadline and diagnostics, and waits for a known warmup TID to
disappear before capturing baseline. It passed 20 sequential and 20 parallel
repetitions. The interpretation is supported by Linux's
[joiner wakeup path](https://github.com/torvalds/linux/blob/v6.18/kernel/fork.c#L1337-L1370)
preceding [later task teardown](https://github.com/torvalds/linux/blob/v6.18/kernel/exit.c#L900-L924).
This was a test-observer race, not a reproduced engine lifetime defect.

The cancelled-sender latch marks entry before `push`; it does not prove the sender
was already asleep inside the condition-variable wait. Pre-cancel/racing-cancel
tests, lifecycle stress and predicate/lock review supplement that coverage.

## Persistence audit

No reproduced storage parser, CRC, endian, allocation-bound or prefix-recovery
defect required a format/source rewrite. Counts are bounded before multiplication
and allocation; frame size is at most 131104 bytes. Header and payload checksums,
reserved words and finite values are checked. New tests use CRC-valid structured
mutations so they reach deeper validation than arbitrary random magic bytes:
30 count/length combinations, every reserved-byte position, signaling/quiet NaNs,
infinities, out-of-domain neighbors and every fresh incomplete-frame prefix.

Complete earlier callbacks can have effects before a later error. Replay has no
rollback, deduplication or resynchronization. Incomplete final headers are ignored
regardless of their contents. CRC is not authentication; stream flush is not
fsync. Single-writer create/truncate and borrowed callback-span rules remain.

## Validation and residual risk

Local final-candidate checks completed on 2026-11-02:

| Configuration | Outcome |
| --- | --- |
| GCC 15.2 Release, strict warnings as errors | 16/16 CTests passed |
| Clang 21.1 Debug, strict warnings as errors | 16/16 CTests passed |
| Clang ASan+UBSan, leak detection and halt-on-error | 16/16 CTests passed; no sanitizer diagnostics |
| Separate Clang TSan, halt-on-error | 16/16 CTests passed; no sanitizer diagnostics |
| GCC RelWithDebInfo, LTO and forced pack fallback | 16/16 CTests passed |
| Fatal Clang static analysis, all six library/CLI/benchmark translation units | No diagnostics; deliberate null-dereference sentinel rejected |

The suite includes the common benchmark harness, fault injection and installed
consumer in every listed build. CI checks the same suite across GCC/Clang
Debug/Release, sanitizers and fallback/LTO; the PR records its completed run.
The local validation result is separate from CI status.

The record in [performance.md](performance.md) contains profiling, controlled
before/after results, exact reproduction commands and permanent raw evidence.
No statistically isolated regression or universal speedup is claimed from shared
WSL scheduling. Production latency budgets still require deployment-specific
measurement. Finite counters, generic payload lock obligations, dense fixed keys,
no live snapshots, no crash durability, and unvalidated native MSVC/complete
32-bit runtime support remain limitations.

Recommendation: merge the reviewed fixes after the final validation/CI is green,
while retaining these limitations. That is a recommendation for this PR, not an
unconditional production sign-off. This audit does not merge the branch.

Library consumers must recompile: synchronous stop-token passage and generic
channel send overloads have changed. Binary compatibility with the original
static library is not promised. The journal v1 wire format is unchanged.
