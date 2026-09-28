#!/usr/bin/env bash
# Builds the release preset and runs the latency and throughput benchmarks for the
# SPSC queue and the mutex baseline. Output goes to docs/results/raw/<timestamp>/.
#
# Usage: scripts/run_bench.sh [producer_cpu] [consumer_cpu] [label_prefix]
#
# Choose CPUs on different physical cores (see the CORE column of `lscpu -e`)
# unless you are deliberately measuring SMT siblings.
set -euo pipefail

cd "$(dirname "$0")/.."
PRODUCER_CPU="${1:-2}"
CONSUMER_CPU="${2:-4}"
PREFIX="${3:-}"
STAMP="$(date +%Y%m%d_%H%M%S)"
OUT="docs/results/raw/$STAMP"
BIN="build/release/bench"
mkdir -p "$OUT"

cmake --preset release >/dev/null
cmake --build --preset release -j >/dev/null

{
  echo "date:      $(date -Iseconds)"
  echo "git:       $(git rev-parse --short HEAD 2>/dev/null || echo n/a)$(git diff --quiet 2>/dev/null || echo '-dirty')"
  echo "cpu:       $(lscpu | sed -n 's/^Model name:[[:space:]]*//p')"
  echo "kernel:    $(uname -r)"
  echo "compiler:  $(g++ --version | head -1)"
  echo "flags:     -O3 -march=native"
  echo "pinning:   producer=$PRODUCER_CPU consumer=$CONSUMER_CPU"
} | tee "$OUT/env.txt"

CPUS=(--producer-cpu="$PRODUCER_CPU" --consumer-cpu="$CONSUMER_CPU")
for q in mutex spsc; do
  "$BIN/bench_latency" --queue=$q --label="${PREFIX}$q" "${CPUS[@]}" \
    --csv="$OUT/latency.csv" --curve="$OUT/latency_curve_${PREFIX}$q.csv" | tee -a "$OUT/log.txt"
done
for q in mutex spsc; do
  "$BIN/bench_throughput" --queue=$q --label="${PREFIX}$q" "${CPUS[@]}" \
    --csv="$OUT/throughput.csv" | tee -a "$OUT/log.txt"
done
echo "results: $OUT"
