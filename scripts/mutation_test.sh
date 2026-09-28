#!/usr/bin/env bash
# Mutation testing for the queue's memory ordering.
# Applies one bug at a time to a throwaway copy of the repo, then checks whether
# (a) the release stress test or (b) TSan catches it. The working tree is never touched.
#
# Usage: scripts/mutation_test.sh [mutation-name ...]    (default: all)
# Env:   MUT_RELEASE_ITEMS (default 20M), MUT_TSAN_ITEMS (default 200k)
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RELEASE_ITEMS="${MUT_RELEASE_ITEMS:-20000000}"
TSAN_ITEMS="${MUT_TSAN_ITEMS:-200000}"
GTEST_SRC="$ROOT/build/debug/_deps/googletest-src"
HDR="include/spsc/spsc_queue.hpp"
# Typed-test instances 0-2 are the SpscQueue capacities (2, 4, 8); 3 is the mutex baseline.
SPSC_FILTER="WraparoundStress/0.*:WraparoundStress/1.*:WraparoundStress/2.*"

# name|perl substitution applied to the header (perl -0pi, so patterns may span lines)
MUTATIONS=(
  "publish_relaxed|s/head_\.store\(head \+ 1, std::memory_order_release\)/head_.store(head + 1, std::memory_order_relaxed)/"
  "consumer_head_relaxed|s/const std::uint64_t head = head_\.load\(std::memory_order_acquire\)/const std::uint64_t head = head_.load(std::memory_order_relaxed)/"
  "producer_tail_relaxed|s/const std::uint64_t tail = tail_\.load\(std::memory_order_acquire\)/const std::uint64_t tail = tail_.load(std::memory_order_relaxed)/"
  "free_slot_relaxed|s/tail_\.store\(tail \+ 1, std::memory_order_release\)/tail_.store(tail + 1, std::memory_order_relaxed)/"
  "publish_before_write|s/(\n\s*buffer_\[head & kMask\] = value;)(.*?)(\n\s*head_\.store\(head \+ 1, std::memory_order_release\);)/\$3\$2\$1/s"
  "free_before_read|s/(\n\s*out = buffer_\[tail & kMask\];)(.*?)(\n\s*tail_\.store\(tail \+ 1, std::memory_order_release\);)/\$3\$2\$1/s"
  "full_check_off_by_one|s/head - tail >= Capacity/head - tail > Capacity/"
)

selected=("$@")
want() { [[ ${#selected[@]} -eq 0 ]] && return 0; local m; for m in "${selected[@]}"; do [[ $m == "$1" ]] && return 0; done; return 1; }

# Runs the stress binary; prints PASS / CAUGHT(<how>).
verdict() {
  local log="$1" rc="$2"
  if grep -q "WARNING: ThreadSanitizer" "$log"; then echo "CAUGHT(tsan-race)"
  elif ! grep -qE "^\[==========\] Running [1-9]" "$log"; then echo "ERROR(no tests ran)"
  elif grep -q "first bad event" "$log"; then echo "CAUGHT(corruption)"
  elif grep -q "stalled after" "$log"; then echo "CAUGHT(stall)"
  elif [[ $rc -ne 0 ]]; then echo "CAUGHT(exit=$rc)"
  else echo "not caught"; fi
}

printf "%-24s | %-20s | %-20s\n" "mutation" "release stress" "tsan"
printf -- "-------------------------+----------------------+---------------------\n"

for entry in "${MUTATIONS[@]}"; do
  name="${entry%%|*}"; expr="${entry#*|}"
  want "$name" || continue

  work="$(mktemp -d)"
  rsync -a --exclude build --exclude .git "$ROOT/" "$work/"
  before="$(md5sum "$work/$HDR")"
  perl -0pi -e "$expr" "$work/$HDR"
  if [[ "$(md5sum "$work/$HDR")" == "$before" ]]; then
    printf "%-24s | %s\n" "$name" "ERROR: pattern did not apply"; rm -rf "$work"; continue
  fi

  results=()
  for preset in release tsan; do
    items=$RELEASE_ITEMS; [[ $preset == tsan ]] && items=$TSAN_ITEMS
    (cd "$work" && cmake --preset "$preset" -DSPSC_BUILD_BENCH=OFF -DSPSC_BUILD_EXAMPLES=OFF \
        -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="$GTEST_SRC" >/dev/null 2>&1 \
      && cmake --build --preset "$preset" --target test_wraparound_stress -j >/dev/null 2>&1) \
      || { results+=("BUILD FAILED"); continue; }
    runner=(); [[ $preset == tsan ]] && runner=(setarch -R)
    SPSC_STRESS_ITEMS=$items TSAN_OPTIONS="halt_on_error=1" \
      timeout 300 "${runner[@]}" "$work/build/$preset/tests/test_wraparound_stress" \
      --gtest_filter="$SPSC_FILTER" >"$work/$preset.log" 2>&1
    results+=("$(verdict "$work/$preset.log" $?)")
  done
  printf "%-24s | %-20s | %-20s\n" "$name" "${results[0]}" "${results[1]}"
  rm -rf "$work"
done
