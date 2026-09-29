# Empty-queue wait strategies

Raw data: [`raw/20260929_072630_wait/`](raw/20260929_072630_wait/). Produced by
`scripts/run_experiments.sh wait`.

The consumer waits on an empty queue using one of the three strategies in
[`wait_strategy.hpp`](../../include/spsc/wait_strategy.hpp). The producer sends one
event per interval (paced load), and latency is one-way push-to-pop time. Both
`SpinThenYield` and `SpinThenPark` use a 128-iteration spin budget, roughly 2 µs of
`pause` on Zen 4. That is shorter than the gap between events at 10k and 100k
events/s, so at those loads the consumer genuinely yields or parks between events.

Samples per run: 100,000 at 10k events/s, 500,000 at 100k/s, 2,000,000 at 1M/s,
after 20,000 warm-up events. Each value is the median of three runs. p99.99 is
omitted because at 10k events/s it would rest on ten samples.

| Strategy | Offered load | p50 | p90 | p99 | p99.9 | Consumer CPU |
|----------|-------------:|----:|----:|----:|------:|-------------:|
| Busy-spin | 10k/s | 107 ns | 442 ns | 5.8 µs | 627 µs | 107% |
| Busy-spin | 100k/s | 102 ns | 261 ns | 7.6 µs | 291 µs | 107% |
| Busy-spin | 1M/s | 98 ns | 229 ns | 7.1 µs | 163 µs | 107% |
| Spin-then-yield | 10k/s | 292 ns | 461 ns | 5.6 µs | 269 µs | 107% |
| Spin-then-yield | 100k/s | 285 ns | 440 ns | 4.3 µs | 66 µs | 108% |
| Spin-then-yield | 1M/s | 121 ns | 397 ns | 5.1 µs | 68 µs | 108% |
| Spin-then-park | 10k/s | 12.4 µs | 21.6 µs | 44.1 µs | 114 µs | **7%** |
| Spin-then-park | 100k/s | 6.2 µs | 12.7 µs | 26.6 µs | 139 µs | **32%** |
| Spin-then-park | 1M/s | 121 ns | 478 ns | 14.1 µs | 128 µs | 105% |

Consumer CPU is `CLOCK_THREAD_CPUTIME_ID` time divided by wall time. Under WSL2 a
thread that never sleeps reads about 107%, because the guest's CPU-time clock and
its monotonic clock run at slightly different rates. Read the column relative to
busy-spin, not as an absolute percentage.

## Findings

**1. Parking trades about 12 µs of median latency for a nearly idle core.** At
10k events/s the parked consumer uses 7% of a core instead of a full one, and each
event pays for a futex wake-up and a reschedule: 12.4 µs at the median, 44 µs at
p99. At 100k events/s the median halves to 6.2 µs while CPU rises to 32%. A
plausible reason is that more events arrive while the consumer is still spinning
or has not finished parking, but the benchmark does not yet separate those cases.

**2. Yielding costs latency and saves no CPU on an otherwise idle machine.**
`sched_yield` returns immediately when nothing else is runnable, so the consumer
still uses a full core (108%). An event that arrives while the consumer is inside
the system call waits for it to return, which roughly triples the median, from
about 100 ns to about 290 ns. Yielding only helps when another thread is waiting
for the core.

**3. At 1M events/s every strategy behaves like busy-spin.** The 1 µs gap between
events is shorter than the 2 µs spin budget, so the consumer never reaches the
yield or park stage, and the medians converge on about 100–120 ns. The park
strategy's higher p99 (14.1 µs vs. 7.1 µs) is most likely the occasional gap
that does exceed the budget, for example after a producer stall, which then
costs a full wake-up.

**4. Busy-spin has the *worst* p99.9 at low load, which is probably an
environment effect.** At 10k events/s busy-spin's p99.9 is 627 µs, against 269 µs
for yield and 114 µs for park, and it varied from 0.2 to 1.2 ms between runs. The
likely cause is the hypervisor preempting a virtual CPU that never goes idle,
but this has not been verified. On bare metal with an isolated core, busy-spin
should have the best tail as well as the best median. This row needs remeasuring
on native Linux before it supports any conclusion.

## Implications

- **Dedicated, isolated core, latency-critical:** busy-spin. It has the lowest
  median at every load, and its cost (one full core) is already paid.
- **Shared cores or intermittent load:** spin-then-park. It gives up about
  12 µs at the median when idle, and nothing under sustained load, because a short
  spin budget keeps it spinning while events keep arriving.
- **Spin-then-yield** has no regime in this data where it beats both
  alternatives.
