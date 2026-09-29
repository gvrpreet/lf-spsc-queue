# Market replay on lob-engine

Raw data: [`raw/20260929_072630_lob/`](raw/20260929_072630_lob/). Produced by
`scripts/run_experiments.sh lob` with a release build configured with
`-DSPSC_LOB_ENGINE_DIR=<checkout>`.

[`examples/lob_replay`](../../examples/lob_replay/) replays a
[lob-engine](https://github.com/gvrpreet/lob-engine) event log through two threads:

```
reader thread (feed handler)  --SpscQueue<Event>-->  engine thread (lob::OrderBook)
```

The engine thread applies each event exactly as lob-engine's single-threaded
replay does. After each run the program compares the final book checksum and
every counter with `replay::replay()` on the same log, and it checks an
order-sensitive hash of the popped event sequence against the source log. It
exits non-zero on any difference. lob-engine's two committed golden logs run as
ctest cases in every build that enables the integration, including
ThreadSanitizer.

lob-engine's `Event` is 32 bytes, so two ring slots share a cache line. The
`spsc-padded` variant places each event in its own 64-byte line.

## Correctness

| Log | Events | Trades | Book checksum | Two-thread result |
|-----|-------:|-------:|---------------|-------------------|
| Nasdaq ITCH, AAPL, 2020-01-30 | 1,993,687 | 0 | `14650fb0739d0383` | identical for every queue variant |
| Synthetic, 5M | 5,000,000 | 3,406,476 | `01e9c2940a0e996f` | identical for every queue variant |
| Synthetic, 50M | 50,000,000 | 29,298,045 | `e45d178766abc830` | identical for every queue variant |

## Throughput

Median of five repetitions, in millions of events per second. The producer and
consumer are pinned to CPU 2 and CPU 4.

| Log | Single thread | SPSC | SPSC, padded slots | Mutex |
|-----|--------------:|-----:|-------------------:|------:|
| AAPL (2.0M events) | 4.90 | 4.18 (−15%) | 4.31 (−12%) | 2.36 (−52%) |
| Synthetic, 5M | 13.67 | 10.16 (−26%) | 10.20 (−25%) | 3.66 (−73%) |
| Synthetic, 50M | 12.15 | 9.26 (−24%) | 9.20 (−24%) | 3.58 (−71%) |

## Findings

**1. The two-thread replay is slower than the single-threaded one, as expected
for this workload.** Reading an event from a memory-mapped log is a copy of 32
bytes, so the reader thread offloads almost no work from the engine. The pipeline
adds a cross-core hand-off per event: on the 50M log, about 26 ns per event
(1/9.26M − 1/12.15M). A second thread pays off only when the producer side does
real work, such as parsing a network feed or decoding ITCH messages. This
benchmark isolates the cost of the hand-off itself. The program also has a
streaming mode, in which the reader thread does buffered file reads and
lob-engine's per-chunk validation. That mode was added after this run and is not
in the recorded data.

**2. The SPSC queue is 2.6–2.8x faster than the mutex queue on the synthetic
logs, and 1.8x faster on the AAPL day.** The engine's per-event work dominates
the AAPL replay, which leaves less room for the queue to matter.

**3. Padding the 32-byte slots makes no measurable difference.** The spread
between SPSC and padded SPSC (at most 3%) is within run-to-run variation. Sharing
a slot line only costs anything when the two threads touch adjacent slots at the
same moment, that is, when the queue is nearly empty. This benchmark does not
record queue depth, so how often that happens here is not measured.

Measured on WSL2; see the [environment caveat](README.md#environment-caveat).
