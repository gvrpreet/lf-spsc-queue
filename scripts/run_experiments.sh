#!/usr/bin/env bash
# Runs the benchmark suites behind docs/results/. Each suite writes CSVs and a log to
# docs/results/raw/<timestamp>_<suite>/.
#
# Usage: scripts/run_experiments.sh [ablations|diag|wait|burst|pipeline|all] ...
#
# Nothing else CPU-heavy should run concurrently: the producer and consumer are
# pinned (default CPUs 2 and 4, separate physical cores; CPU 3 is CPU 2's SMT sibling).
# The script refuses to start if the 1-minute load average suggests other work.
set -euo pipefail

load="$(cut -d' ' -f1 /proc/loadavg)"
if [[ -z "${FORCE:-}" ]] && awk -v l="$load" 'BEGIN { exit !(l > 2.0) }'; then
  echo "load average is $load: other work is running and would distort results (FORCE=1 to override)" >&2
  exit 3
fi

cd "$(dirname "$0")/.."
P="${PRODUCER_CPU:-2}"
C="${CONSUMER_CPU:-4}"
SMT="${SMT_SIBLING_CPU:-3}"
LAT_RUNS="${LAT_RUNS:-3}"
BIN="build/release/bench"
STAMP="$(date +%Y%m%d_%H%M%S)"

cmake --preset release >/dev/null
cmake --build --preset release -j >/dev/null

write_env() {
  {
    echo "date:      $(date -Iseconds)"
    echo "git:       $(git rev-parse --short HEAD 2>/dev/null || echo n/a)$(git diff --quiet 2>/dev/null || echo '-dirty')"
    echo "cpu:       $(lscpu | sed -n 's/^Model name:[[:space:]]*//p')"
    echo "kernel:    $(uname -r)"
    echo "compiler:  $(g++ --version | head -1)"
    echo "flags:     -O3 -march=native"
    echo "pinning:   producer=$P consumer=$C smt_sibling=$SMT"
  } >"$1/env.txt"
}

suite_dir() {
  local d="docs/results/raw/${STAMP}_$1"
  mkdir -p "$d"
  write_env "$d"
  echo "$d"
}

ablations() {
  local d; d="$(suite_dir ablations)"
  local variants=(mutex a0 a1 spsc pad128 prefetch seqcst)
  for q in "${variants[@]}"; do
    "$BIN/bench_throughput" --queue="$q" --producer-cpu="$P" --consumer-cpu="$C" \
      --csv="$d/throughput.csv" | tee -a "$d/log.txt"
  done
  for q in a0 spsc; do  # A6: producer and consumer on SMT siblings
    "$BIN/bench_throughput" --queue="$q" --label="$q-smt" --producer-cpu="$P" --consumer-cpu="$SMT" \
      --csv="$d/throughput.csv" | tee -a "$d/log.txt"
  done
  for run in $(seq "$LAT_RUNS"); do
    for q in "${variants[@]}"; do
      "$BIN/bench_latency" --queue="$q" --producer-cpu="$P" --consumer-cpu="$C" \
        --csv="$d/latency.csv" --curve="$d/curve_${q}_run${run}.csv" | grep -v '^#' | tee -a "$d/log.txt"
    done
    for q in a0 spsc; do
      "$BIN/bench_latency" --queue="$q" --label="$q-smt" --producer-cpu="$P" --consumer-cpu="$SMT" \
        --csv="$d/latency.csv" | grep -v '^#' | tee -a "$d/log.txt"
    done
  done
}

wait_strategies() {
  local d; d="$(suite_dir wait)"
  # interval_ns:samples -> 10k/s, 100k/s, 1M/s offered load (~10 s, 5 s, 2 s per run).
  # A 128-iteration spin budget (~2 µs of pause on Zen 4) is shorter than every gap,
  # so yield/park genuinely give up the CPU between events.
  local loads=(100000:100000 10000:500000 1000:2000000)
  for run in $(seq "$LAT_RUNS"); do
    for w in spin yield park; do
      for l in "${loads[@]}"; do
        "$BIN/bench_latency" --queue=spsc --wait="$w" --spin=128 --interval-ns="${l%%:*}" \
          --samples="${l##*:}" --warmup=20000 --producer-cpu="$P" --consumer-cpu="$C" \
          --csv="$d/latency.csv" | grep -v '^#' | tee -a "$d/log.txt"
      done
    done
  done
}

# Throughput diagnostics: how often each side finds the queue full/empty, and how
# throughput responds when the consumer backs off before re-reading head_.
diagnostics() {
  local d; d="$(suite_dir diag)"
  for b in 1 8 32 128; do
    for q in a0 a1 spsc seqcst; do
      "$BIN/bench_throughput" --queue="$q" --label="$q/backoff$b" --empty-backoff="$b" --seconds=2 \
        --reps=3 --producer-cpu="$P" --consumer-cpu="$C" --csv="$d/throughput.csv" | tee -a "$d/log.txt"
    done
  done
}

burst() {
  local d; d="$(suite_dir burst)"
  for q in mutex spsc; do
    # Bursts that fit in the queue: 4096 events at 50M/s offered, 1 ms gaps.
    "$BIN/bench_burst" --queue="$q" --label="$q-fit" --bursts=1000 --burst-size=4096 \
      --spacing-ns=20 --gap-us=1000 --producer-cpu="$P" --consumer-cpu="$C" \
      --csv="$d/burst.csv" | grep -v '^#' | tee -a "$d/log.txt"
    # Bursts larger than the queue (65,536) with a slow consumer: forces backpressure.
    "$BIN/bench_burst" --queue="$q" --label="$q-overflow" --bursts=20 --burst-size=200000 \
      --spacing-ns=20 --gap-us=50000 --service-ns=100 --producer-cpu="$P" --consumer-cpu="$C" \
      --csv="$d/burst.csv" | grep -v '^#' | tee -a "$d/log.txt"
  done
}

pipeline() {
  local d; d="$(suite_dir pipeline)"
  for rate in 1000000 0; do
    build/release/examples/order_pipeline/order_pipeline --requests=5000000 --rate="$rate" \
      --producer-cpu="$P" --consumer-cpu="$C" | tee -a "$d/log.txt"
  done
}

suites=("$@")
[[ ${#suites[@]} -eq 0 || ${suites[0]} == all ]] && suites=(ablations diag wait burst pipeline)
for s in "${suites[@]}"; do
  case "$s" in
    ablations) ablations ;;
    diag) diagnostics ;;
    wait) wait_strategies ;;
    burst) burst ;;
    pipeline) pipeline ;;
    *) echo "unknown suite: $s" >&2; exit 2 ;;
  esac
done
