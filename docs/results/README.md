# Benchmark Results

Every figure quoted in the top-level README comes from a file in this directory.
Every file here is produced by a script in `scripts/`.

## Layout

| Path | Contents | Produced by |
|------|----------|-------------|
| `raw/<timestamp>/env.txt` | Machine, kernel, compiler, git revision, pinning | `scripts/run_bench.sh` |
| `raw/<timestamp>/latency.csv` | Latency percentile summary per queue | `bench_latency --csv` |
| `raw/<timestamp>/latency_curve_*.csv` | Full percentile curve (for plotting) | `bench_latency --curve` |
| `raw/<timestamp>/throughput.csv` | Throughput median/min/max per queue | `bench_throughput --csv` |
| `baseline_vs_spsc.md` | Headline comparison against the mutex baseline | Curated from `raw/` |
| `mutation_testing.md` | Which injected ordering bugs the test suite detects | `scripts/mutation_test.sh` |

## Methodology

- Release build, `-O3 -march=native`, GCC 13.3.
- Producer and consumer pinned to different physical cores (CPU 2 and CPU 4)
  unless stated otherwise.
- **Latency:** one-way push-to-pop time measured with the invariant TSC, under
  paced load (one event per microsecond). The timestamp is taken once, before the
  first push attempt, so time spent blocked on a full queue or a held lock is
  included. 5,000,000 samples after 500,000 warm-up events.
- **Throughput:** saturating producer, 3-second runs, 5 repetitions after a
  discarded warm-up run. Reported as median with min/max. Every event is
  verified by checksum.
- **Queue capacity:** 65,536 slots for both the SPSC queue and the mutex baseline.

## Environment caveat

These measurements are taken on WSL2, a Hyper-V virtual machine on Windows 11.
The host can deschedule virtual CPUs, and `isolcpus`/`nohz_full` are
unavailable, so tail percentiles (p99.9 and above) are noisier than on bare-metal
Linux. Comparisons between configurations measured back to back on the same
machine remain meaningful; absolute tail values should be read with that caveat.
