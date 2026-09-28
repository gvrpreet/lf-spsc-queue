# Mutation testing

`scripts/mutation_test.sh` copies the repository to a temporary directory,
applies a single bug to `include/spsc/spsc_queue.hpp`, and runs the wraparound
stress test (SPSC capacities 2, 4, and 8; unpinned, separate cores, and SMT
siblings) twice:

- **Release stress:** `-O3 -march=native`, 20M events per test.
- **ThreadSanitizer:** 200k events per test.

A mutation is *caught* if either run reports a data race, a corrupted, lost, or
reordered event, or a stall.

## Results

| Mutation | Description | Release stress | ThreadSanitizer |
|----------|-------------|----------------|-----------------|
| `publish_relaxed` | Producer publishes `head_` with `relaxed` instead of `release` | not caught | **caught** (race) |
| `consumer_head_relaxed` | Consumer loads `head_` with `relaxed` instead of `acquire` | not caught | **caught** (race) |
| `producer_tail_relaxed` | Producer loads `tail_` with `relaxed` instead of `acquire` | not caught | **caught** (race) |
| `free_slot_relaxed` | Consumer frees the slot with `relaxed` instead of `release` | not caught | **caught** (race) |
| `publish_before_write` | Producer publishes `head_` before writing the slot | **caught** (corruption) | **caught** (race) |
| `free_before_read` | Consumer publishes `tail_` before reading the slot | **caught** (corruption) | **caught** (race) |
| `full_check_off_by_one` | Full check `>` instead of `>=` (overwrites an unread slot) | **caught** (corruption) | **caught** (race) |

All seven mutations are detected.

## Interpretation

- **Weakened orderings pass the release stress test on x86-64.** Under x86-TSO,
  the hardware never reorders a store with an earlier store or a load with an
  earlier load. GCC also happened not to reorder the relaxed accesses in these
  builds. The bugs are real under the C++ memory model, and they would surface on
  ARM or POWER, or after an unrelated code change gives the compiler room to reorder.
  Only a tool that checks the language-level happens-before relation finds them on
  this machine. This is why ThreadSanitizer is a required gate, not an optional one.
- **Reordering the statements themselves is caught by both.** Those bugs are
  wrong on any hardware, and the stress test observes the corruption directly. The
  tiny capacities make the full/empty boundary collide constantly, which is what
  exposes them within a few million events.
- **`producer_tail_relaxed`** is the ordering most often argued to be "safe
  enough". ThreadSanitizer flags it as a write-after-read race on the slot, which
  confirms the analysis in [ADR-0002](../adr/0002-memory-ordering.md).
