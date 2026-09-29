# Bursty load

Raw data: [`raw/20260929_043739_burst/`](raw/20260929_043739_burst/). Produced by
`scripts/run_experiments.sh burst`.

The producer follows a fixed schedule: bursts of events spaced 20 ns apart (50M
events/s offered, faster than either queue can drain), separated by idle gaps.
Latency is measured from each event's **intended** send time in the schedule. If
the producer is held up by a full queue or a held lock, every event scheduled
during the stall is charged for it, so there is no coordinated omission. The
producer pushes through the full-queue policy (`push_with_backpressure`: spin,
yield, explicit reject after 1 ms).

## Scenario 1: bursts that fit in the queue

1,000 bursts × 4,096 events, 1 ms gaps, no consumer work per event.

| Queue | p50 | p90 | p99 | p99.9 | Max depth | Full events | Rejected |
|-------|-----|-----|-----|-------|-----------|-------------|----------|
| `std::mutex` + `std::queue` | 401 µs | 583 µs | 928 µs | 1.78 ms | 8,088 | 0 | 0 |
| SPSC (default tuning) | **41 µs** | **116 µs** | **626 µs** | 1.78 ms | 8,129 | 0 | 0 |

A burst arrives in 82 µs. The SPSC queue drains it in roughly 180 µs, while the
mutex queue takes roughly 700 µs, so events wait correspondingly longer in the
mutex queue. The median is **9.8x lower** with the SPSC queue. The identical
p99.9 and the maximum depth of about two bursts in *both* queues come from the
same cause: an occasional vCPU stall of about 1 ms that let two bursts
accumulate. That is an environment effect, not a queue effect.

## Scenario 2: bursts larger than the queue, slow consumer

20 bursts × 200,000 events (3x the 65,536 capacity), 50 ms gaps, 100 ns of
simulated work per event in the consumer.

| Queue | p50 | p90 | p99 | Max depth | Full events | Rejected |
|-------|-----|-----|-----|-----------|-------------|----------|
| `std::mutex` + `std::queue` | 478 ms | 852 ms | 970 ms | 65,536 | 1,192,330 | 7 |
| SPSC (default tuning) | **12.8 ms** | **23.0 ms** | **26.0 ms** | 65,536 | 2,040,145 | 0 |

Both queues fill, and the backpressure policy engages: millions of full-queue
events, every one counted. The difference is what happens *while* the queue is
full:

- **SPSC:** each rejected `try_push` reads only the consumer's index. The
  consumer keeps running at full speed, each burst drains within its gap, and
  latency stays bounded by one burst's backlog. No order was rejected.
- **Mutex:** each rejected `try_push` *acquires the lock*. A producer retrying
  against a full queue competes for the same mutex the consumer needs to make
  room, so the consumer slows down exactly when it most needs to speed up. The
  backlog never drains between bursts, latency accumulates across the run to
  nearly a second, and 7 orders exceeded the 1 ms budget and were rejected
  explicitly (counted and reported, never silently dropped).

This is the strongest practical argument for the lock-free design: under
overload, a lock-based queue's backpressure mechanism slows down its own consumer.
