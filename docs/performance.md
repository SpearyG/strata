# Measured performance and validation

These are observations on one shared desktop, not service-level objectives or
cross-machine comparisons. Measurements were taken on 2026-11-01 after real
Release builds. Raw temporary JSONL and build products are deliberately excluded
from Git. The tables below transcribe the measured sample output.

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
