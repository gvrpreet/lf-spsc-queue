# Benchmark Results

Every figure quoted in the top-level README comes from a file in this directory.
Every file here is produced by a script in `scripts/`.

## Layout

| Path | Contents | Produced by |
|------|----------|-------------|
| [`ablations.md`](ablations.md) | Every queue configuration vs. the mutex baseline: latency, throughput, analysis | `scripts/run_experiments.sh ablations` |
| [`burst.md`](burst.md) | Bursty load, including bursts larger than the queue | `scripts/run_experiments.sh burst` |
| [`wait_strategies.md`](wait_strategies.md) | Busy-spin vs. spin-then-yield vs. spin-then-park: latency and consumer CPU at three loads | `scripts/run_experiments.sh wait` |
| [`lob_replay.md`](lob_replay.md) | Two-thread replay on lob-engine: bit-identical results, SPSC vs. mutex | `scripts/run_experiments.sh lob` |
| [`pipeline.md`](pipeline.md) | End-to-end order pipeline (intake to acknowledgement) | `scripts/run_experiments.sh pipeline` |
| [`mutation_testing.md`](mutation_testing.md) | Which injected bugs each verification method catches | `scripts/mutation_test.sh` |
| [`baseline_vs_spsc.md`](baseline_vs_spsc.md) | First comparison, before the layout optimizations (historical) | `scripts/run_bench.sh` |
| `plots/*.svg` | Charts used in the README and in `ablations.md` | `scripts/plot_results.py` |
| `raw/<timestamp>_<suite>/` | Raw CSVs, logs, and `env.txt` (machine, kernel, compiler, git revision, pinning) | the scripts above |

## Methodology

- Release build, `-O3 -march=native`, GCC 13.3.
- Producer and consumer pinned to different physical cores (CPU 2 and CPU 4)
  unless stated otherwise.
- **Latency:** one-way push-to-pop time measured with the invariant TSC
  (`lfence; rdtsc` on both sides), under paced load (one event per microsecond).
  The timestamp is taken once, before the first push attempt, so time spent
  blocked on a full queue or a held lock is included. 5,000,000 samples after
  500,000 warm-up events; median of three runs.
- **Bursty load:** latency is measured from each event's *intended* send time in a
  precomputed schedule, which avoids coordinated omission.
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
