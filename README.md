# lf-spsc-queue

A bounded, lock-free, single-producer/single-consumer (SPSC) ring buffer in
C++20 that decouples an **order-intake thread** (parse, validate, build
`OrderEvent`) from a **matching-engine thread** (apply to the book, generate
trades and acknowledgements). It is benchmarked against a `std::mutex` +
`std::queue` baseline and exercised end to end by an order-pipeline example
with a price-time-priority order book.

The core queue is a single header, [`include/spsc/spsc_queue.hpp`](include/spsc/spsc_queue.hpp).

## Headline results

Ryzen 7 7840HS (Zen 4), WSL2, GCC 13.3 `-O3 -march=native`. Producer and consumer
are pinned to separate physical cores, and both queues hold 65,536 events.

| | `std::mutex` + `std::queue` | SPSC queue | |
|---|---|---|---|
| Hand-off latency p50 (1 event/µs) | 510 ns | **98 ns** | 5.2x lower |
| Hand-off latency p90 | 1,908 ns | **207 ns** | 9.2x lower |
| Hand-off latency p99 | 24.7 µs | **5.1 µs** | 4.8x lower |
| Sustained throughput | 6.2 M events/s | **22.9 M events/s** | 3.7x higher |
| Bursts of 4,096 at 50 M/s, p50 wait | 401 µs | **41 µs** | 9.8x lower |
| Bursts of 3x capacity with slow consumer, p50 wait | 478 ms | **12.8 ms** | 37x lower; 7 vs. 0 orders rejected |

In the order pipeline, a request's median time from arrival at intake to
acknowledgement (validation, queue hand-off, and order-book update) is **184 ns**
at 1M requests/s. The matching thread sustains at least 15M requests/s.

Latency is the median of three runs of 5M samples. Percentiles above p99 are
dominated by WSL2 virtual-CPU scheduling and are not compared. Details:
[ablations](docs/results/ablations.md), [bursty load](docs/results/burst.md),
[pipeline](docs/results/pipeline.md).

![Latency percentiles](docs/results/plots/latency_percentiles.svg)

## Thread model

```
 Intake thread (producer)                                            Matching thread (consumer)
 parse -> validate -> build OrderEvent  --try_push-->  [ ring ]  --try_pop-->  apply to book -> trades/acks
                                                         |
                                  head_: written only by the producer
                                  tail_: written only by the consumer
```

- Exactly one producer thread and one consumer thread. Each index has a single
  writer, so no compare-and-swap is needed anywhere.
- Events are stored **by value** in a power-of-two array of 64-byte,
  cache-line-aligned slots. There is no heap allocation on the hot path.
- The indices are monotonically increasing 64-bit counters (`slot = index & mask`),
  so all slots are usable and full/empty are unambiguous.
- Each index lives on its own cache line together with its owner's cached copy
  of the other index. The remote line is read only when the cache says the queue
  is full (producer) or empty (consumer).
- `try_push` and `try_pop` never block. Full-queue and empty-queue behavior are
  separate, explicit policies layered on top.

## Memory ordering

| Access | Order | Reason |
|--------|-------|--------|
| Load own index | `relaxed` | Only this thread writes it |
| Load the other side's index | `acquire` | Synchronizes with the other side's release; makes slot contents (producer to consumer) and slot reuse (consumer to producer) race-free |
| Publish own index | `release` | Publishes the slot write, or the completed slot read, to the other side |

The producer's load of `tail_` is often argued to be safe with `relaxed`. It is
not safe under the C++ memory model. Without an acquire, nothing orders the
consumer's read of a slot before the producer's overwrite of it. That is a data
race, even though x86 hardware would never exhibit it. On x86, `acquire` compiles
to the same plain `mov` as `relaxed`, so the correct choice is free. The generated
code contains no fences.

Full reasoning, including why the design is immune to ABA:
[docs/design/DESIGN.md](docs/design/DESIGN.md) and
[ADR-0002](docs/adr/0002-memory-ordering.md).

## Full-queue policy

Silently dropping an order is never acceptable in a trading system. `try_push`
only reports that the queue is full. The policy layer,
[`push_with_backpressure`](include/spsc/backpressure.hpp), then works in stages:

1. It spins briefly with `pause`, retrying the push.
2. It yields the CPU, retrying, for a bounded time budget.
3. If the budget runs out, it rejects the event explicitly, so the caller can
   reject the order back to the client and raise an alert.

Every full-queue event and every rejection is counted. The invariant, verified
by a test with a throttled consumer, is *delivered + rejected == sent*. The
bursty-load benchmark shows why the lock-free design matters here: when the queue
is full, a mutex-based producer's retries compete for the same lock the consumer
needs to make room. See [ADR-0001](docs/adr/0001-full-queue-policy.md) and
[bursty load](docs/results/burst.md).

## Empty-queue wait strategies

[`wait_strategy.hpp`](include/spsc/wait_strategy.hpp) provides three strategies
behind one interface (`pop(queue, out)` for the consumer, `notify()` for the producer):

- **Busy-spin:** lowest latency, one full core.
- **Spin-then-yield.**
- **Spin-then-park:** futex via `std::atomic::wait`. Idle CPU near zero.

Parking uses a Dekker-style handshake with seq_cst fences on both sides, so a
wake-up can never be lost. Mutation testing shows that removing either fence
produces real lost wake-ups on x86, which ThreadSanitizer cannot detect. See
[ADR-0003](docs/adr/0003-wait-strategy.md).

## Usage

```cpp
#include "spsc/spsc_queue.hpp"
#include "spsc/backpressure.hpp"

auto q = std::make_unique<spsc::SpscQueue<spsc::OrderEvent, 65536>>();  // 4 MiB: keep off the stack

// Producer thread
spsc::PushStats stats;
if (!spsc::push_with_backpressure(*q, event, stats)) {
    reject_to_client(event);  // explicit, counted; never silent
}

// Consumer thread
spsc::OrderEvent ev;
while (q->try_pop(ev)) book.apply(ev);
```

Each optimization can be switched off at compile time, so it can be measured in
isolation: `SpscQueue<T, N, spsc::Tuning{.index_align = 8, .cache_remote_index = false}>`.

## Verification

- **Unit tests** check the API contract for every queue configuration and for the
  mutex baseline: FIFO order, exact capacity, rejection on full, and wraparound.
- **Stress tests** push millions of checksummed, sequence-numbered events through
  queues of capacity 2 to 8. They run unpinned, pinned to separate physical
  cores, and pinned to SMT siblings. Any lost, duplicated, reordered, or torn event
  fails the test.
- **Wait-strategy tests** ping-pong single events with randomized gaps, so the
  consumer is caught spinning, parking, and parked. A lost wake-up becomes a
  detected stall.
- **ThreadSanitizer and AddressSanitizer/UBSan** builds run the full suite.
- **Mutation testing** ([`scripts/mutation_test.sh`](scripts/mutation_test.sh))
  injects 15 ordering and protocol bugs; all 15 are caught. Relaxed-ordering bugs
  pass x86 stress tests and are caught only by ThreadSanitizer. Removed fences
  in the park protocol are caught only by the stress tests.
  [Details](docs/results/mutation_testing.md).

## Building

Requirements: Linux x86-64, GCC 13+ or Clang 17+, CMake 3.25+. GoogleTest is
fetched automatically.

```bash
cmake --preset release && cmake --build --preset release
scripts/check_gates.sh                 # tests under debug, ThreadSanitizer, AddressSanitizer
scripts/run_experiments.sh all         # every benchmark suite behind docs/results/
scripts/mutation_test.sh               # mutation testing
build/release/examples/order_pipeline/order_pipeline --requests=5000000 --rate=1000000
```

## Repository layout

```
include/spsc/     spsc_queue.hpp       lock-free SPSC ring buffer (+ Tuning knobs)
                  order_event.hpp      64-byte order event
                  backpressure.hpp     full-queue policy
                  wait_strategy.hpp    busy-spin / spin-yield / spin-park
                  mutex_queue.hpp      mutex + std::queue baseline
common/           platform helpers (TSC, CPU pinning)
tests/            unit, stress, wait-strategy, and order-book tests (GoogleTest)
bench/            latency, throughput, and bursty-load benchmarks
examples/         order pipeline: intake -> queue -> matching engine
scripts/          gates, experiments, mutation testing, plotting
docs/design/      design document
docs/adr/         architecture decision records
docs/results/     benchmark and mutation-testing results
```

## Environment caveat

All measurements were taken on WSL2 (a Hyper-V virtual machine). Medians and p90
are stable across runs. Tail percentiles, and the effect of SMT placement,
require bare-metal Linux with isolated cores to measure credibly.

## License

Released under the [MIT License](LICENSE).
