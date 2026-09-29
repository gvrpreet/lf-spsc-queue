# Mutation testing

`scripts/mutation_test.sh` copies the repository to a temporary directory,
applies a single bug, and runs the relevant test twice: once as an optimized
release build and once under ThreadSanitizer. A mutation is *caught* if either
run reports a data race, a corrupted, lost, or reordered event, or a stall
(detected by the tests' progress watchdog).

| Target | Test | Release run | TSan run |
|--------|------|-------------|----------|
| Queue (`spsc_queue.hpp`) | `test_wraparound_stress`: default queue at capacities 2, 4, 8 plus the uncached baseline path; unpinned, separate cores, SMT siblings | 20M events per test | 200k events per test |
| Park protocol (`wait_strategy.hpp`) | `test_wait_strategy` ping-pong: one event in flight, randomized gaps so the consumer is spinning, parking, or parked | 20k round trips | 20k round trips |

## Results: 15 of 15 mutations caught

### Queue memory ordering

| Mutation | Description | Release | TSan |
|----------|-------------|---------|------|
| `publish_relaxed` | Producer publishes `head_` with `relaxed` | not caught | **caught** (race) |
| `free_slot_relaxed` | Consumer frees the slot with `relaxed` | not caught | **caught** (race) |
| `cached_consumer_head_relaxed` | Consumer refreshes its cached `head_` with `relaxed` | not caught | **caught** (race) |
| `cached_producer_tail_relaxed` | Producer refreshes its cached `tail_` with `relaxed` | not caught | **caught** (race) |
| `uncached_consumer_head_relaxed` | Baseline path: consumer loads `head_` with `relaxed` | not caught | **caught** (race) |
| `uncached_producer_tail_relaxed` | Baseline path: producer loads `tail_` with `relaxed` | not caught | **caught** (race) |
| `publish_before_write` | `head_` published before the slot is written | **caught** (corruption) | **caught** (race) |
| `free_before_read` | `tail_` published before the slot is read | **caught** (corruption) | **caught** (race) |
| `cached_full_check_off_by_one` | Full check `>` instead of `>=` (cached path) | **caught** (corruption) | **caught** (race) |
| `uncached_full_check_off_by_one` | Full check `>` instead of `>=` (baseline path) | **caught** (corruption) | **caught** (race) |
| `cache_never_refreshed` | Producer never re-reads `tail_` after its cache says "full" | **caught** (stall) | **caught** (stall) |

### Park/notify protocol (`SpinThenPark`)

| Mutation | Description | Release | TSan |
|----------|-------------|---------|------|
| `park_no_notify` | Producer never wakes a parked consumer | **caught** (stall) | **caught** (stall) |
| `park_skip_recheck` | Consumer parks without re-checking the queue | **caught** (stall) | **caught** (stall) |
| `park_no_consumer_fence` | seq_cst fence removed between `sleeping_ = true` and the re-check | **caught** (stall) | not caught |
| `park_no_producer_fence` | seq_cst fence removed between publishing and reading `sleeping_` | **caught** (stall) | not caught |

## Interpretation

The two groups fail in opposite ways, and together they show why both kinds of
verification are required.

- **Weakened acquire/release orderings in the queue pass the release stress test
  on x86-64.** Under x86-TSO the hardware never reorders load-load, store-store,
  or load-store, so the bug cannot manifest on this CPU. It is nonetheless a data
  race under the C++ memory model. It would surface on ARM or POWER, or after an
  unrelated change gives the compiler room to reorder. ThreadSanitizer checks the
  language-level happens-before relation, and it is the only tool that finds these.
- **Removing a seq_cst fence from the park protocol produces real lost wake-ups
  on x86-64**, and ThreadSanitizer misses them. The protocol is a Dekker-style
  store-then-load handshake on two variables, and x86 *does* reorder a store with
  a later load through the store buffer. The consumer's `sleeping_ = true` can
  still be buffered when it re-checks the queue, while the producer reads a stale
  `sleeping_`. Neither side sees the other, and the consumer sleeps with an event
  queued. ThreadSanitizer does not report it because there is no data race (every
  shared access is atomic) and TSan does not model store-buffer reordering. Only
  the ping-pong stress test, which turns a lost wake-up into a detectable stall,
  catches it.
- **Statement reorderings and off-by-one errors are caught by both.** They are
  wrong on any hardware, and the tiny capacities used in the stress tests hit the
  full/empty boundary constantly.

Conclusion: ThreadSanitizer, correctness-checking stress tests, and a watchdog
that converts hangs into failures are complementary. Each catches a class of bug
the others miss.
