# Performance experiments (ablations)

Each configuration changes one thing relative to the one before it (see the
experiment table in [DESIGN.md §7](../design/DESIGN.md#7-performance-experiments)).
Raw data: [`raw/20260929_043739_ablations/`](raw/20260929_043739_ablations/).
Produced by `scripts/run_experiments.sh ablations`. Charts: `scripts/plot_results.py`.

Environment: Ryzen 7 7840HS (Zen 4), WSL2 (kernel 6.18), GCC 13.3 `-O3 -march=native`.
Producer on CPU 2, consumer on CPU 4 (separate physical cores) unless noted. Queue
capacity 65,536 × 64-byte events.

## Latency (push to pop, 1 event/µs, 5M samples per run, median of 3 runs, ns)

| ID | Configuration | p50 | p90 | p99 |
|----|---------------|-----|-----|-----|
| | `std::mutex` + `std::queue` | 510 | 1,908 | 24,687 |
| A0 | SPSC baseline: indices share a cache line, no cached indices | 107.5 | 243.5 | 4,950 |
| A1 | + `head_`/`tail_` on separate 64-byte lines | 102.7 | 270.8 | 4,832 |
| A2 | + cached remote indices (**default**) | **98.3** | **206.6** | 5,123 |
| | A2 with 128-byte isolation | 103.3 | 201.1 | 4,736 |
| A4 | A2 + consumer prefetch of the next slot | 98.6 | **169.0** | 4,334 |
| A5 | A2 with `seq_cst` for every atomic operation | 102.8 | 205.8 | 4,711 |
| A6 | A0 with the consumer on CPU 3 (SMT sibling of CPU 2) | 107.5 | 271.6 | 4,955 |
| A6 | A2 with the consumer on CPU 3 | 98.6 | 281.0 | 34,928 |

![Latency percentiles](plots/latency_percentiles.svg)

## Throughput (saturating producer, 5 × 3 s runs, M events/s)

| ID | Configuration | Median | Min | Max |
|----|---------------|--------|-----|-----|
| | `std::mutex` + `std::queue` | 6.19 | 6.12 | 6.45 |
| A0 | SPSC baseline | 23.57 | 22.97 | 25.34 |
| A1 | + index padding | 21.32 | 20.12 | 22.25 |
| A2 | + cached indices (default) | 22.86 | 21.74 | 23.55 |
| | 128-byte isolation | 23.23 | 21.99 | 24.00 |
| A4 | + consumer prefetch | 23.44 | 22.33 | 23.85 |
| A5 | `seq_cst` everywhere | **48.99** | 48.10 | 49.21 |
| A6 | A0 on SMT siblings | 24.12 | 23.98 | 25.81 |
| A6 | A2 on SMT siblings | 23.58 | 23.43 | 24.49 |

![Throughput](plots/throughput.svg)

## Findings

**1. Latency: the tuned layout is about 9% faster at p50 and 15% at p90.** The
hand-off cost is dominated by moving the slot's cache line (64 bytes) from the
producer's core to the consumer's. Isolating the indices and caching the remote
index remove the *extra* index-line transfers, which is visible but secondary.
Consumer prefetch of the next slot lowered p90 further, to 169 ns (−18% vs. A2,
consistent across the two clean runs).

**2. 128-byte isolation brings nothing over 64 bytes on Zen 4.** 64-byte
separation is sufficient on this CPU. 128 bytes guards against adjacent-line
prefetch pairing on some Intel parts, so it remains a portability choice
rather than a measured win here.

**3. Throughput: padding and cached indices did *not* raise saturated
throughput, while `seq_cst` doubled it. The mechanism is the consumer outrunning
the producer.** A follow-up run with full/empty retry counters (added to
`bench_throughput`) showed that in every acquire/release variant the consumer
finds the queue *empty* 0.2–0.6 times per event, so it is faster than the
producer. Every empty check re-reads `head_`, pulling the producer's index line
into the consumer's cache. The producer's next `head_` store must then take the
line back before it can retire, and its store buffer backs up behind that
ownership request. Cached indices cannot help in this regime: the cache is only
consulted when the consumer has caught up, which is exactly when it must be refreshed.

With `seq_cst`, the consumer's `tail_` store becomes a locked `xchg`, a full
barrier that drains the store buffer. That slows the consumer just enough that it
stops catching up. Mean queue depth rose from about 4,000 to about 34,500, empty
retries fell about 10x, the two cores stopped contending for the same lines, and
throughput doubled. `seq_cst` is not faster in itself: it accidentally
implements consumer back-off.

*Status:* this explanation fits the retry counters and queue depths. The
confirming experiment, sweeping an explicit consumer back-off
(`bench_throughput --empty-backoff=N`) across all variants, is pending a quiet
machine and will be recorded here. The practical conclusion does not depend on
it: for throughput, the consumer should back off or batch when it finds the
queue empty instead of hammering the producer's index. For latency,
acquire/release with immediate re-checks remains the right default.

**4. SMT placement shows no effect under WSL2.** Logical CPUs 2 and 3 are SMT
siblings in the guest topology, but Hyper-V does not guarantee that the two vCPUs
run on sibling hyperthreads of the same physical core at any moment. The A6 rows
are inconclusive rather than negative, and need bare-metal Linux to measure.

**5. Tail percentiles (p99.9 and above) are not comparable between rows.** They
are dominated by vCPU descheduling in the virtual machine. The third latency run
of several configurations was hit by a multi-millisecond stall. Medians of three
runs are reported so that a single disturbed run cannot move a row.
