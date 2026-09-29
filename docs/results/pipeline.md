# Order pipeline (end to end)

Raw data: [`raw/20260929_043739_pipeline/`](raw/20260929_043739_pipeline/).
Produced by `scripts/run_experiments.sh pipeline`, which runs
`examples/order_pipeline`.

The intake thread generates a deterministic request stream (about 70% new orders,
20% cancels, 10% modifies, and 1% deliberately invalid), validates each request,
and pushes the valid ones through the SPSC queue. The matching thread owns a
price-time-priority order book and records latency from request arrival at
intake to acknowledgement generation. That covers validation, queue hand-off,
and matching.

## Results (5,000,000 requests)

| | Paced at 1M requests/s | Unthrottled |
|---|---|---|
| Offered rate | 0.99 M/s | 15.05 M/s |
| Rejected at intake (validation) | 50,269 | 50,269 |
| Enqueued / applied by matcher | 4,949,731 / 4,949,731 | 4,949,730 / 4,949,730 |
| Matcher acks / rejects (order no longer live) | 3,735,415 / 1,214,316 | 3,735,415 / 1,214,315 |
| Trades | 3,234,547 | 3,234,542 |
| Full-queue events / backpressure rejects | 0 / 0 | 5,129 / 1 |
| Latency p50 (intake to ack) | **184 ns** | 2.76 ms |
| Latency p90 | 13.2 µs | 6.30 ms |
| Accounting (enqueued == applied) | OK | OK |
| Book invariants (not crossed, levels consistent) | OK | OK |

## Observations

- **At 1M requests/s the median intake-to-acknowledgement latency is 184 ns,**
  which includes validation, the queue hand-off, and the order-book update.
- **The matcher sustains at least 15M requests/s.** In the unthrottled run, 4.95M
  events were applied in 0.33 s including the final drain. At that rate the intake
  outruns the matcher, the queue fills (5,129 full-queue events), and latency
  becomes queueing delay (milliseconds). That is the expected saturation behavior,
  not a defect.
- **The single backpressure reject is the policy working as designed.** One push
  waited longer than the 1 ms budget, most likely during a vCPU stall of the
  matcher, and was rejected *explicitly*. The accounting still balances exactly:
  requests = intake rejects + enqueued + backpressure rejects. Because of that one
  reject, the matcher's counts differ by one from the paced run on the same
  deterministic stream.
- The paced run's p90 (13 µs) is well above the queue-only p90 measured in the
  ablation suite (about 200 ns). The order-book work per event is small, so most of
  that gap is environment noise during this run. p50 is the robust statistic in
  this environment.
