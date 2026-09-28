#!/usr/bin/env bash
# Correctness gates: build + test under debug, tsan, asan.
# Usage: scripts/check_gates.sh [ctest -R regex]
set -uo pipefail

cd "$(dirname "$0")/.."
FILTER="${1:-}"
status=0

for preset in debug tsan asan; do
  echo "== $preset"
  cmake --preset "$preset" >/dev/null || { echo "configure failed"; status=1; continue; }
  if ! build_log="$(cmake --build --preset "$preset" -j 2>&1)"; then
    echo "$build_log" | grep -E 'error' | head -20
    status=1
    continue
  fi
  echo "$build_log" | grep -E 'warning:' | sort -u | head -20
  ctest --preset "$preset" ${FILTER:+-R "$FILTER"} 2>&1 \
    | grep -E 'Failed|tests passed|ThreadSanitizer|AddressSanitizer|runtime error' || true
  [[ ${PIPESTATUS[0]} -eq 0 ]] || status=1
done

exit "$status"
