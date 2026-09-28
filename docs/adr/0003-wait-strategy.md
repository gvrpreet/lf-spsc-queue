# ADR-0003: Empty-queue wait strategy

- **Status:** Proposed
- **Date:** 2026-09-28

## Context

When `try_pop` returns false, the consumer must wait. The choice trades wake-up
latency against CPU consumption. The matcher is latency-critical, but a
development machine and a production host with isolated cores have very
different CPU budgets. The wait strategy is therefore a template parameter
rather than a fixed choice.

## Options considered

| Strategy | Wake-up latency | Idle CPU | Notes |
|----------|-----------------|----------|-------|
| Busy-spin (`pause` loop) | Lowest: one cache-line transfer | 100% of a core | Requires an isolated core; competes with the SMT sibling. |
| Spin, then `sched_yield` | Low while spinning, then scheduler-dependent | High | `sched_yield` returns immediately if nothing else is runnable. |
| Spin, then park (futex via `std::atomic::wait`) | Microseconds after parking | Near zero | The producer must notify a parked consumer without making a syscall on every push. |

## Park/notify protocol

- **Consumer:** set `sleeping = true`, re-check the queue, and wait only if it is still empty.
- **Producer:** publish `head_`, then check `sleeping`. If it is set, clear it and notify.

This is a store-then-load pattern on two different variables in each thread:
the consumer stores `sleeping` and loads `head_`, and the producer stores
`head_` and loads `sleeping`. Release/acquire does not prevent both loads from
observing stale values, which would lose a wake-up. The two stores and two
loads therefore need `seq_cst` ordering, or a `seq_cst` fence between each
store and load.

## Decision

Pending. The decision will be based on measured latency, throughput, and consumer CPU
usage for each strategy at low, medium, and saturating load.
