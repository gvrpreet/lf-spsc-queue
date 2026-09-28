# ADR-0001: Full-queue policy

- **Status:** Accepted (budget values provisional)
- **Date:** 2026-09-28

## Context

`try_push` returns `false` when the ring is full, and the caller decides what
happens next. In a trading system, silently dropping an order is unacceptable:
the client believes the order is live, risk state diverges from reality, and
regulatory obligations may be breached. Any policy must therefore make every
undelivered order loud, counted, and observable.

Forces:
- The producer usually also reads from the network. If it blocks for too long,
  kernel socket buffers fill up. TCP then pushes back on the client; UDP drops
  packets, which moves the silent loss one layer down instead of removing it.
- The consumer may stall temporarily: a page fault, a cache-cold book level, a
  large sweeping order, or preemption on a non-isolated core.
- Order flow and market data arrive in bursts.

## Options considered

1. **Silent drop.** Rejected outright.
2. **Blocking backpressure (bounded spin, then yield).** The producer retries
   with `pause`, then `sched_yield`, until space frees up. Every full event is
   counted. No loss, but latency moves upstream and socket buffers can overflow.
3. **Generous sizing, with "full" treated as an alarm.** Size the ring so it
   should never fill (for example 65,536 slots x 64 B = 4 MiB). If it does
   fill, reject the order explicitly back to the client (`REJECT: SYSTEM_BUSY`),
   count it, and raise an alert. Producer latency stays bounded and the client
   is informed, but rejections are client-visible.
4. **Hybrid.** Spin for a short, bounded budget, then reject explicitly and
   raise an alarm. Loss is never silent.

## Decision

Option 4, the hybrid, implemented in `include/spsc/backpressure.hpp`:

1. **Fast path.** A single `try_push`, with no timing and no extra shared-memory traffic.
2. **Spin.** Up to `spin_limit` (default 1024) `pause`-spins, retrying the push.
   This absorbs short consumer hiccups at the lowest hand-off latency.
3. **Yield.** Yield the CPU, retrying, for up to `reject_after` (default 1 ms).
4. **Reject.** The event is returned to the caller as rejected, and
   `PushStats::rejected` is incremented. The caller must reject the order back to
   the client and raise an alert.

Every slow-path entry is counted in `PushStats::full_events`, so persistent
pressure is visible to monitoring long before rejections begin. Setting
`reject_after` to zero gives pure blocking backpressure (option 2) for
deployments that prefer to push back through TCP.

The default budgets are provisional. They should be set from the consumer stall
distribution measured under bursty load.

## Sizing

Required capacity is at least (burst arrival rate - service rate) x burst
duration, plus headroom. This will be computed from measured burst profiles.

## Evidence

- `tests/test_full_queue.cpp` throttles the consumer and verifies that
  delivered + rejected == sent, that ordering is preserved, and that full events are counted.
- Queue-depth high-water marks under bursts will be recorded in `docs/results/`.
