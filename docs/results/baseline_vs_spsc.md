# SPSC queue vs. mutex baseline

> Historical record (2026-09-28): the first comparison, made before the layout
> optimizations. The configuration measured here is now available as
> `SpscQueue<T, N, kBaselineTuning>`. Current figures for every configuration,
> including the tuned default, are in [ablations.md](ablations.md).

Configuration: `SpscQueue<OrderEvent, 65536>` with the baseline layout A0 (indices
on a shared cache line, no cached indices) against `MutexQueue<OrderEvent, 65536>`
(`std::mutex` + `std::queue`). Raw data: [`raw/20260928_125316/`](raw/20260928_125316/).

## Environment

```
cpu:       AMD Ryzen 7 7840HS (Zen 4, 8 cores / 16 threads), TSC 3.82 GHz, invariant
kernel:    6.18.33.2-microsoft-standard-WSL2 (WSL2 on Windows 11)
compiler:  g++ 13.3.0, -O3 -march=native
pinning:   producer CPU 2 (core 1), consumer CPU 4 (core 2); separate physical cores
timer:     lfence+rdtsc, 38 cycles (~10 ns) per read
```

## Latency (push to pop, paced at 1 event/µs, 5M samples per run, 3 runs)

Median across runs, with the min-max range in parentheses. All values in nanoseconds.

| Percentile | Mutex baseline | SPSC queue | Ratio (median) |
|------------|----------------|------------|----------------|
| p50 | 542 (542-543) | 114 (109-118) | 4.8x lower |
| p90 | 2,413 (1,941-2,477) | 243 (189-349) | 9.9x lower |
| p99 | 19,754 (19,133-28,313) | 5,497 (3,537-25,120) | 3.6x lower |
| p99.9 | 111,685 (102,582-347,229) | 146,212 (44,972-3,127,807) | not comparable |
| p99.99 | 514,646 (416,352-635,405) | 573,018 (278,786-6,230,945) | not comparable |

No negative TSC deltas were observed in any run.

## Throughput (saturating producer, 5 x 3 s runs after a discarded warm-up)

| Queue | Median | Min | Max |
|-------|--------|-----|-----|
| Mutex baseline | 6.15 M events/s | 6.10 M | 6.20 M |
| SPSC queue | 24.28 M events/s | 23.73 M | 25.12 M |

The SPSC queue sustains **3.9x** the throughput of the mutex baseline. Each
event is a 64-byte `OrderEvent`, so the SPSC figure is about 1.55 GB/s of payload.

## Interpretation

- **Median and p90.** The SPSC hand-off costs roughly one cross-core cache-line
  transfer for the slot plus one for the index. The mutex adds lock acquisition
  and release on both sides, contention on the mutex's cache line, and
  `std::deque` chunk allocation. The p90 gap (about 10x) is wider than the p50 gap
  because the mutex occasionally falls into the kernel (futex) under contention.
- **Tails (p99.9 and above).** At these percentiles the samples are dominated by
  the virtualization layer, not by either queue. The Hyper-V host deschedules
  vCPUs for hundreds of microseconds to milliseconds. At 1 event/µs pacing, a
  3 ms stall leaves thousands of events queued behind it. Every one of those
  events records the stall, which is why a single bad run moves p99.9 by two
  orders of magnitude. Tail comparisons need bare-metal Linux with isolated cores.
- **This is the unoptimized layout.** Both indices share one cache line, and
  every operation reads the other side's index. Those are the targets of
  ablations A1 (padding) and A2 (cached indices) in the design document.
