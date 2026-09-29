#!/usr/bin/env bash
# Mutation testing for the queue's memory ordering and the park/notify protocol.
# Applies one bug at a time to a throwaway copy of the repo, then checks whether
# (a) a release build of the relevant test or (b) its ThreadSanitizer build catches
# it. The working tree is never touched.
#
# Usage: scripts/mutation_test.sh [mutation-name ...]    (default: all)
# Env:   MUT_RELEASE_ITEMS (default 20M), MUT_TSAN_ITEMS (default 200k)
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RELEASE_ITEMS="${MUT_RELEASE_ITEMS:-20000000}"
TSAN_ITEMS="${MUT_TSAN_ITEMS:-200000}"
GTEST_SRC="$ROOT/build/debug/_deps/googletest-src"
RLX="std::memory_order_relaxed"

QUEUE_HDR="include/spsc/spsc_queue.hpp"
# Instances 0-2: default (cached-index) queue at capacities 2, 4, 8; 3: uncached path.
QUEUE_TEST="test_wraparound_stress|WraparoundStress/0.*:WraparoundStress/1.*:WraparoundStress/2.*:WraparoundStress/3.*"
WAIT_HDR="include/spsc/wait_strategy.hpp"
# Instance 2 is SpinThenPark; the ping-pong test turns a lost wake-up into a stall.
WAIT_TEST="test_wait_strategy|WaitStrategyTest/2.PingPongNoLostWakeup"

# name|file|test-binary|gtest-filter|perl substitution (perl -0pi: patterns may span lines)
MUTATIONS=(
  # Queue: shared by both paths
  "publish_relaxed|$QUEUE_HDR|$QUEUE_TEST|s/head_\.store\(head \+ 1, kRelease\)/head_.store(head + 1, $RLX)/"
  "free_slot_relaxed|$QUEUE_HDR|$QUEUE_TEST|s/tail_\.store\(tail \+ 1, kRelease\)/tail_.store(tail + 1, $RLX)/"
  "publish_before_write|$QUEUE_HDR|$QUEUE_TEST|s/(\n\s*buffer_\[head & kMask\] = value;)(.*?)(\n\s*head_\.store\(head \+ 1, kRelease\);)/\$3\$2\$1/s"
  "free_before_read|$QUEUE_HDR|$QUEUE_TEST|s/(\n\s*out = buffer_\[tail & kMask\];)(.*?)(\n\s*tail_\.store\(tail \+ 1, kRelease\);)/\$3\$2\$1/s"
  # Queue: cached-index path (default tuning)
  "cached_consumer_head_relaxed|$QUEUE_HDR|$QUEUE_TEST|s/cached_head_ = head_\.load\(kAcquire\)/cached_head_ = head_.load($RLX)/"
  "cached_producer_tail_relaxed|$QUEUE_HDR|$QUEUE_TEST|s/cached_tail_ = tail_\.load\(kAcquire\)/cached_tail_ = tail_.load($RLX)/"
  "cached_full_check_off_by_one|$QUEUE_HDR|$QUEUE_TEST|s/head - cached_tail_ >= Capacity/head - cached_tail_ > Capacity/g"
  "cache_never_refreshed|$QUEUE_HDR|$QUEUE_TEST|s/\n\s*cached_tail_ = tail_\.load\(kAcquire\);//"
  # Queue: uncached path (baseline tuning)
  "uncached_consumer_head_relaxed|$QUEUE_HDR|$QUEUE_TEST|s/const std::uint64_t head = head_\.load\(kAcquire\)/const std::uint64_t head = head_.load($RLX)/"
  "uncached_producer_tail_relaxed|$QUEUE_HDR|$QUEUE_TEST|s/const std::uint64_t tail = tail_\.load\(kAcquire\)/const std::uint64_t tail = tail_.load($RLX)/"
  "uncached_full_check_off_by_one|$QUEUE_HDR|$QUEUE_TEST|s/head - tail >= Capacity/head - tail > Capacity/"
  # Park/notify protocol (SpinThenPark)
  "park_no_notify|$WAIT_HDR|$WAIT_TEST|s/if \(sleeping_\.load\(std::memory_order_relaxed\)\) wake_all\(\);//"
  "park_skip_recheck|$WAIT_HDR|$WAIT_TEST|s/(std::atomic_thread_fence\(std::memory_order_seq_cst\);\n\s*)if \(q\.try_pop\(out\)\) \{/\$1if (false) {/"
  "park_no_consumer_fence|$WAIT_HDR|$WAIT_TEST|s/(sleeping_\.store\(true, std::memory_order_relaxed\);\n)\s*std::atomic_thread_fence\(std::memory_order_seq_cst\);\n/\$1/"
  "park_no_producer_fence|$WAIT_HDR|$WAIT_TEST|s/(void notify\(\) noexcept \{\n)\s*std::atomic_thread_fence\(std::memory_order_seq_cst\);\n/\$1/"
)

selected=("$@")
want() { [[ ${#selected[@]} -eq 0 ]] && return 0; local m; for m in "${selected[@]}"; do [[ $m == "$1" ]] && return 0; done; return 1; }

# Classifies a test log; prints "not caught" or CAUGHT(<how>).
verdict() {
  local log="$1" rc="$2"
  if grep -q "WARNING: ThreadSanitizer" "$log"; then echo "CAUGHT(tsan-race)"
  elif ! grep -qE "^\[==========\] Running [1-9]" "$log"; then echo "ERROR(no tests ran)"
  elif grep -qE "first bad event|EXPECT_EQ\(bad" "$log"; then echo "CAUGHT(corruption)"
  elif grep -qE "stalled (after|at)" "$log"; then echo "CAUGHT(stall)"
  elif [[ $rc -eq 124 ]]; then echo "CAUGHT(timeout)"
  elif [[ $rc -ne 0 ]]; then echo "CAUGHT(exit=$rc)"
  else echo "not caught"; fi
}

printf "%-32s | %-20s | %-20s\n" "mutation" "release" "tsan"
printf -- "---------------------------------+----------------------+---------------------\n"

for entry in "${MUTATIONS[@]}"; do
  IFS='|' read -r name file target filter expr <<<"$entry"
  want "$name" || continue

  work="$(mktemp -d)"
  rsync -a --exclude build --exclude .git "$ROOT/" "$work/"
  before="$(md5sum "$work/$file")"
  perl -0pi -e "$expr" "$work/$file"
  if [[ "$(md5sum "$work/$file")" == "$before" ]]; then
    printf "%-32s | %s\n" "$name" "ERROR: pattern did not apply"; rm -rf "$work"; continue
  fi

  results=()
  for preset in release tsan; do
    items=$RELEASE_ITEMS; [[ $preset == tsan ]] && items=$TSAN_ITEMS
    (cd "$work" && cmake --preset "$preset" -DSPSC_BUILD_BENCH=OFF -DSPSC_BUILD_EXAMPLES=OFF \
        -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="$GTEST_SRC" >/dev/null 2>&1 \
      && cmake --build --preset "$preset" --target "$target" -j >/dev/null 2>&1) \
      || { results+=("BUILD FAILED"); continue; }
    runner=(); [[ $preset == tsan ]] && runner=(setarch -R)
    SPSC_STRESS_ITEMS=$items TSAN_OPTIONS="halt_on_error=1" \
      timeout 300 "${runner[@]}" "$work/build/$preset/tests/$target" \
      --gtest_filter="$filter" >"$work/$preset.log" 2>&1
    results+=("$(verdict "$work/$preset.log" $?)")
  done
  printf "%-32s | %-20s | %-20s\n" "$name" "${results[0]}" "${results[1]}"
  rm -rf "$work"
done
