# ADR-0003: Empty-queue wait strategy

- **Status:** Proposed (implemented; the latency/CPU comparison is pending)
- **Date:** 2026-09-29

## Context

When `try_pop` returns false, the consumer must wait. The choice trades wake-up
latency against CPU consumption. The matcher is latency-critical, but a
development machine and a production host with isolated cores have very
different CPU budgets. The wait strategy is therefore a type parameter rather
than a fixed choice.

## Options considered

| Strategy | Wake-up latency | Idle CPU | Notes |
|----------|-----------------|----------|-------|
| `BusySpin` (`pause` loop) | Lowest: one cache-line transfer | 100% of a core | Requires an isolated core; competes with the SMT sibling. |
| `SpinThenYield` | Low while spinning, then scheduler-dependent | High | `sched_yield` returns immediately if nothing else is runnable. |
| `SpinThenPark` (futex via `std::atomic::wait`) | Microseconds after parking | Near zero | The producer must wake a parked consumer without making a syscall on every push. |

All three share one interface: the consumer calls `wait.pop(queue, out)`, which
returns once an element has been popped, and the producer calls `wait.notify()`
after each push. The interface takes the queue rather than exposing an
`idle()` hook, because parking must re-check the queue *after* announcing sleep.

## Park/notify protocol

- **Consumer:** load `epoch_` (acquire); set `sleeping_`; **seq_cst fence**;
  re-check the queue; if it is still empty, `epoch_.wait(epoch)`.
- **Producer:** publish `head_` (release); **seq_cst fence**; if `sleeping_` is
  set, increment `epoch_` (release) and `notify_one()`.

This is a Dekker-style store-then-load handshake on two variables. The consumer
stores `sleeping_` and then loads `head_`. The producer stores `head_` and then
loads `sleeping_`. Release/acquire does not order a store before a later load,
so without the fences both loads could see stale values: the consumer would miss
the element and the producer would miss the sleeper, a lost wake-up. With a
seq_cst fence between the store and the load on each side, at least one of them
is guaranteed to see the other's store.

The kernel wait is keyed on a separate `epoch_` counter that the consumer reads
*before* announcing sleep. A notify that lands between the re-check and the
`wait()` call changes the epoch, so `wait()` returns immediately instead of sleeping.

**Cost:** the producer executes one seq_cst fence (`mfence` on x86) per
`notify()`. It makes a syscall only when the consumer is actually parked.

## Verification

- `test_wait_strategy` ping-pongs single events with randomized gaps (none,
  sub-microsecond, 20 µs, 50 µs sleep), so the consumer is caught spinning,
  parking, and parked. With one event in flight, a lost wake-up cannot be rescued
  by a later push, and it shows up as a stall. The test also asserts that the park
  path was actually exercised.
- Mutation testing removed each fence in turn. Both mutations produced *real*
  lost wake-ups on x86-64, and were caught by the stress test but *not* by
  ThreadSanitizer. The store-load reordering happens in the store buffer, which
  TSan does not model, and there is no data race to report
  ([results](../results/mutation_testing.md)).

## Decision

Pending: the latency, throughput, and consumer-CPU comparison at 10k, 100k, and
1M events/s (`scripts/run_experiments.sh wait`).

Expected guidance, to be confirmed by the measurements:
- `BusySpin` on isolated cores for production matching.
- `SpinThenPark` where cores are shared or load is intermittent.
