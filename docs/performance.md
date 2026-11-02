# Measured performance and validation

These are observations on one shared desktop, not service-level objectives or
cross-machine comparisons. Measurements were taken on 2026-11-01 after real
Release builds. Build products and temporary measurements are excluded from Git.
Selected permanent raw evidence is retained under `docs/benchmarks/` for the audit
below. The original tables transcribe the initial delivery's measured output.

## Environment

- AMD Ryzen 5 5600X, 6 cores / 12 logical CPUs, x86-64.
- WSL2 Linux `6.18.54.1-microsoft-standard-WSL2`, Ubuntu 26.04.1 LTS.
- 7.7 GiB available VM RAM, glibc 2.43, libstdc++ 15.
- GCC `15.2.0 (Ubuntu 15.2.0-16ubuntu1)`.
- Clang `21.1.8 (6ubuntu1)` for sanitizer and analysis checks.
- CMake 4.2.3, Ninja, `-std=c++26`, Release `-O3 -DNDEBUG`.
- LTO and `-march=native` disabled for these measurements.
- No affinity, isolated CPUs, fixed frequency or dedicated host. Scheduler,
  host load and virtualization can substantially affect individual samples.

## Reproduction and timing

```sh
cmake -S . -B build/release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-15
cmake --build build/release -j
./build/release/strata_bench --events 1000000 --workers 4 --producers 4 \
  --queue-capacity 4096 --batch-size 64 --series 1024 --seed 42 \
  --repeats 7 --warmups 1
```

Each executable run performs one unreported warmup pair and seven measured
pairs, alternating which variant runs first. Every sample processes 1,000,000
events with zero rejection/discard and verifies accepted/processed counts and a
reproducible source fingerprint. Latency instrumentation is enabled. The source
does no optional CPU mixing or burst sleeping (`work=0`, `burst=0`).

Timing includes producer thread startup, generation, per-event fingerprinting,
queue backpressure and worker drain/join. Engine construction, allocation,
snapshot copying and JSON output are outside the measured interval. This is an
end-to-end synthetic ingestion benchmark, not an isolated channel microbenchmark.
The fingerprint validates the source; per-series aggregate correctness is checked
separately by tests against a reference. Replays and disk I/O are not benchmarked.

For longer steady-state measurements, increase `--events` substantially. The
one-million-event samples are short and show broad scheduling variation; seven
repeats provide distribution evidence, not statistical confidence bounds.

## Optimization experiment

The pre-optimization baseline is commit `982fdb3`. The measured implementation
after the optimization is `5afcbb9`; the subsequent parser-only fix does not alter
this workload. In libstdc++ 15, the stop-aware condition-variable wait constructs
a stop callback and accesses its shared synchronization state even if the queue
predicate is already true. The workload supplies a stop-capable token on every
submission. This made cancellation registration part of the common ready-queue
path.

The change checks queue readiness while holding the existing queue mutex and
enters the stop-aware wait only when the ring is full. It retains the explicit
stop check before acceptance, so pre-cancellation, close precedence and
linearization semantics remain intact. No notification policy, payload,
aggregation or benchmark timing changed. TSan and shutdown tests were rerun.

| Implementation | Dequeue batch | Median events/s | Minimum | Maximum |
| --- | ---: | ---: | ---: | ---: |
| Before ready-path change | 1 | 2,206,504 | 2,076,502 | 2,342,836 |
| Before ready-path change | 64 | 2,304,532 | 2,095,650 | 2,472,302 |
| After ready-path change | 1 | 4,602,007 | 2,006,500 | 6,753,560 |
| After ready-path change | 64 | 5,139,834 | 3,399,344 | 5,791,148 |

For batch 64, the two session medians differ by 2.23x. Within the optimized
session, batch 64's median exceeds batch 1's by 11.7%. The before/after sessions
were sequential, not interleaved builds; environmental drift may contribute to
the difference. Do not interpret the ratio as a guaranteed speedup. No hardware
profile or contention trace was collected: the explanation comes from library
implementation inspection and the targeted measured change.

All seven batch-64 samples, rounded to whole events/s:

| Sample | Before | After |
| --- | ---: | ---: |
| 0 | 2,095,650 | 3,399,344 |
| 1 | 2,424,537 | 5,588,410 |
| 2 | 2,167,282 | 5,791,148 |
| 3 | 2,472,302 | 5,139,834 |
| 4 | 2,286,215 | 3,815,497 |
| 5 | 2,304,532 | 5,382,721 |
| 6 | 2,376,040 | 3,538,694 |

## Latency

Submission-attempt-to-aggregate latency includes admission wait and queue residence.
The histogram uses logarithmic nanosecond buckets. Percentiles below are the
**median of seven per-run bucket upper bounds**, not pooled event percentiles.
The maximum is the worst observed value across those seven runs.

| Batch-64 implementation | p50 upper | p95 upper | p99 upper | Observed maximum |
| --- | ---: | ---: | ---: | ---: |
| Before | 16.383 us | 131.071 us | 262.143 us | 1,379.914 us |
| After | 8.191 us | 131.071 us | 524.287 us | 2,134.311 us |

The optimized run's tail bounds worsened while throughput increased. Faster
admission can build bursts and increase queueing; host scheduling also contributes.
The current experiment cannot separate these causes. Use smaller queues/batches
and a workload representative of the application's latency budget before choosing
a deployment topology. No hard realtime or p99 guarantee is claimed.

## Remaining bottlenecks and limits

- Every producer still locks the target queue for each event. Producer-side
  batching could reduce this, but complicates partial acceptance/cancellation.
- Multiple producers contending on a hot series share one shard. Work stealing
  conflicts with the lock-free ownership of summaries.
- Batch dequeue wakes all waiting producers, which can cause a thundering herd.
- Two clock reads per event and histogram recording are part of the measured
  default; use `--no-latency` to investigate their cost rather than silently
  removing instrumentation from comparisons.
- The source generator/fingerprint and thread startup consume measured time.
  This benchmark does not establish the maximum throughput of aggregation alone.
- The scalar CRC32C journal implementation favors portability; hardware CRC
  dispatch, journal throughput and disk durability need separate measurement.

## Verification record

The development tree passed GCC Debug and Release builds and Clang strict builds.
A clean clone at `8548991` passed all 11 CTest targets in each of three independently
configured builds: GCC Release, Clang ASan+UBSan (with leak detection), and Clang
TSan. Separate GCC RelWithDebInfo + LTO + forced pack fallback passed all 11 tests.
Clang static analysis of every library source emitted no diagnostics. An external
CMake client consumed the installed package successfully. Six extra CLI validation
cases returned JSON errors, and a real SIGINT run exited 130 with consistent
accepted/processed/discarded accounting.

The [first complete GitHub Actions run](https://github.com/SpearyG/strata/actions/runs/41344480959)
passed all seven jobs: GCC/Clang Debug/Release, ASan+UBSan, TSan, and fallback/LTO
with static analysis. Later documentation and analysis-runner changes do not alter
the measured C++ implementation. The final delivery reports the final commit's
clean builds, sanitizer checks and additional longer benchmark separately.

Coverage includes finite/NaN/overflow validation, pipeline short circuit, channel
FIFO/wraparound/close/cancel, movable ownership, MPMC exact transfer, randomized
multi-producer topologies, concurrent shutdown, idle workers, source-independent
reference aggregation, journal roundtrips and all frame truncation prefixes,
bit corruption, malicious lengths, 10,000 randomized malformed buffers, I/O and
callback failure paths, CLI replay, and benchmark execution.

## Adversarial audit: retained 5M-event evidence

The original delivery's five-million-event result is now retained as
[legacy-final-5m.jsonl](benchmarks/legacy-final-5m.jsonl), associated with baseline
`848d59e`. Its SHA256 is
`626d9fbe36aa177d7f766d1afc0ce28da587bdb032bfd1316a11e5fa5c4bfe46`.
Recomputing all seven samples verifies the originally reported batch-64 median
6,630,558.642 events/s, range 5,130,357.990..6,783,617.480, median per-run p99
upper bound 131071ns and worst observed maximum 8996404ns. Every recorded run
accepted and processed 5M events with zero rejected/discarded.

The same original harness and baseline were rebuilt and rerun with the command
below. [Rerun evidence](benchmarks/legacy-rerun-5m.jsonl) gives batch-64 median
6,073,047.726 events/s, range 5,371,160.910..6,573,040.609; batch-1 median was
5,825,348.683. This verifies retained historical arithmetic and provides a new
observation. It does **not** reproduce the old throughput exactly or turn it into
a guarantee. The original source checksum omitted series/timestamp; that gap is
fixed in the audit. Historical data is not relabeled with the new checksum.

```sh
# In a detached checkout of 848d59e, GCC 15 Release, no LTO/native flags:
./build/release/strata_bench --events 5000000 --workers 4 --producers 4 \
  --queue-capacity 4096 --batch-size 64 --series 1024 --seed 42 \
  --repeats 7 --warmups 1
```

## Controlled engine-revision comparison

The audit uses one `benchmarks/audit/ingest.cpp` translation unit compiled against
each revision's engine and headers: before `848d59e`, after `8105e61`. Both use GCC
15.2, C++26, `-g -O3 -DNDEBUG`, no LTO/native flags, on the environment above.
Later delivery changes add tests/docs/analysis coverage, not measured runtime
behavior. Binary and harness hashes, refs, workload parameters, warmups, measured
samples and order are recorded. See [manifest](benchmarks/manifest.json).

`tools/compare_audit.py` runs a warmup for each binary, then seven measured pairs
in alternating BA/AB order, rather than two separated build sessions. Both are
affined to CPUs 0..11. These CPUs are shared, not isolated; host frequency and
Windows/WSL scheduling remain uncontrolled. No confidence interval or dedicated
host guarantee is asserted.

Generation, complete-payload source checksums, producer startup, backpressure and
drain/join are timed. A long-double per-series reference is computed before
timing; snapshot checks and output are after timing. Every measured run requires
complete unique in-range series identities, exact counts, finite moments, means
and M2 within documented harness tolerances, exact extrema, correct latency count,
zero rejects/discards and an identical source checksum across both binaries.
All aggregate fields feed an observable digest. Floating moments/digests can
differ in final bits across schedules, so only reference tolerances are compared.
This harness differs from the legacy application harness; compare its two engine
revisions with each other, not their absolute rates with the historical table.

Primary retained results from the hardened validation harness:

| Workload | Before median events/s [min, max] | After median events/s [min, max] | Median change |
| --- | ---: | ---: | ---: |
| Uniform, 5M, 4 producers/4 workers, capacity4096, batch64, latency on | 5,861,115 [5,269,719, 6,576,327] | 6,198,078 [5,140,486, 7,248,517] | +5.75% |
| Hot key, 5M, same topology/instrumentation | 6,298,641 [6,006,238, 6,551,148] | 6,486,714 [5,988,094, 6,547,634] | +2.99% |
| Uniform, 10M, 8 producers/4 workers, capacity65536, batch64, latency off | 6,410,406 [4,157,608, 8,123,577] | 8,172,912 [5,831,189, 9,339,645] | +27.49% |

Raw files: [uniform](benchmarks/audit-uniform-5m.jsonl),
[hot key](benchmarks/audit-hot-key-5m.jsonl),
[latency disabled](benchmarks/audit-no-latency-10m.jsonl).
There are two retained warmups and fourteen measured samples in each file.
Every sample processed exactly its requested 5M or 10M observations.

The earlier audit session is also retained under `initial-audit-*`: uniform
5.831M ->5.962M (+2.26%), hot key6.501M ->6.514M (+0.20%), and latency-disabled
6.688M ->9.683M (+44.79%). Those runs preceded additional outside-timing NaN and
series-identity validation; generation, timing and engine behavior are unchanged.
Both sessions support a workload-specific benefit for reduced token copying.
Default/hot-key ranges overlap substantially: a small median difference is not
evidence of a universal speedup. No systematic throughput regression was observed
in the measured cases; untested workloads may behave differently.

Latency below is the median of seven per-run percentile **upper bounds**, with
the worst observed maximum. No tail improvement is promised:

| Primary workload | Variant | p50 upper ns | p95 upper ns | p99 upper ns | Worst maximum ns |
| --- | --- | ---: | ---: | ---: | ---: |
| Uniform5M | Before | 8191 | 131071 | 524287 | 4255371 |
| Uniform5M | After | 8191 | 131071 | 524287 | 8309536 |
| Hot key5M | Before | 32767 | 262143 | 524287 | 6533456 |
| Hot key5M | After | 32767 | 131071 | 262143 | 6072001 |

The uniform run's worst maximum worsened despite higher median throughput. The
earlier hot-key session had an after maximum of12,189,607ns. Scheduler outliers
and burst queueing remain material; a deployment latency target needs dedicated
measurement. Latency-disabled runs report zero samples/percentiles, not zero
actual latency. Changes in topology/instrumentation across workload rows prevent
using them to isolate instrumentation overhead by taking a simple ratio.

Reproduce the controlled method from this branch on Linux:

```sh
mkdir -p build
git worktree add --detach build/audit-base 848d59e83de36597dd02220eaeef6aea41496a15
cmake -S benchmarks/audit -B build/measure-before -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-15 -DCMAKE_CXX_FLAGS=-g \
  -DSTRATA_ENGINE_SOURCE_ROOT="$PWD/build/audit-base"
cmake -S benchmarks/audit -B build/measure-after -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-15 -DCMAKE_CXX_FLAGS=-g \
  -DSTRATA_ENGINE_SOURCE_ROOT="$PWD"
cmake --build build/measure-before -j
cmake --build build/measure-after -j
python3 tools/compare_audit.py \
  --before build/measure-before/ingest --after build/measure-after/ingest \
  --before-ref 848d59e83de36597dd02220eaeef6aea41496a15 --after-ref "$(git rev-parse HEAD)" \
  --repeats 7 --warmups 1 --cpu-list 0-11 -- \
  --events 5000000 --workers 4 --producers 4 --capacity 4096 --batch 64 --series 1024
```

Use `--hot-key` for skew. For the third case set `--events 10000000 --producers 8
--capacity 65536 --no-latency`. Adjust or omit `--cpu-list` on machines without
these twelve logical CPUs. The common harness is also built/tested as
`strata_audit_ingest` in normal, sanitizer and LTO builds.

## Profiling evidence and retained optimization

Before changing token passage, a baseline GCC Release+`-g` CLI ran20M events under
`perf stat` and flat199Hz user-cycle sampling. The stat run reported2.795s wall,
16.251B cycles,12.960B instructions,432.489M cache references,118.618M cache misses,
2.124B branches and23.331M branch misses. These are virtualized PMU observations,
not a causal proof of a cache bottleneck. The user-filtered context-switch count
was zero and must not be read as evidence of no kernel scheduling activity.
See [stat output](benchmarks/profile-stat.txt) and [flat samples](benchmarks/profile-flat.txt).

The installed perf6.18 build lacks libelf, libdw and DWARF unwind support. A DWARF
record attempt failed with `Bad address`; flat `cycles:u` recording succeeded with
3069 samples and zero reported lost samples. `addr2line -f -i -C` resolved program
offsets against the debug binary:0x9ba0 (3.79% flat samples) was stop-state owner
increment inside engine submission;0x9dad (3.12%) and0xf2e1 (2.20%) were stop-token
ownership release sites. vDSO samples were13.47%, consistent with clock reads but
not fully symbolized; libc offsets resolved to
mutex/condition-variable lock operations. Unknown/kernel entries in the report
and missing unwind support limit attribution. No complete flamegraph or precise
lock-wait profile is claimed.

The retained optimization passes stop tokens by const reference only through
synchronous submit/channel calls. It removes ready-path shared ownership copies;
a blocking standard-library wait still takes its required copy. No token is
stored or borrowed past the call. Queue layout, scheduling and notification policy
were not changed. The controlled results above, especially the latency-disabled
case across two sessions, justify retaining this small change. No speculative
lock-free rewrite or wakeup micro-optimization was added.

Example profiling commands, using a baseline CLI built with `-g`:

```sh
perf stat -e task-clock,context-switches,cycles,instructions,cache-references,cache-misses,branches,branch-misses \
  ./build/profile/strata --events 20000000 --workers 4 --producers 4
perf record -e cycles:u -F 199 -o /tmp/strata-audit.data \
  ./build/profile/strata --events 20000000 --workers 4 --producers 4
perf report --stdio --no-children --percent-limit 1 -i /tmp/strata-audit.data
```

The initial verification record above describes the first delivery. The current
[adversarial audit](audit.md) and PR report the expanded final checks separately.
