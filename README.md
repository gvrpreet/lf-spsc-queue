# lf-spsc-queue

A bounded, lock-free, single-producer/single-consumer (SPSC) ring buffer in
C++20 that decouples an **order-intake thread** (parse, validate, build
`OrderEvent`) from a **matching-engine thread** (apply to the book, generate
trades and acknowledgements). It is benchmarked against a `std::mutex` +
`std::queue` baseline.

The core queue is a single header, [`include/spsc/spsc_queue.hpp`](include/spsc/spsc_queue.hpp).

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
trade-offs against pure blocking backpressure and against generous sizing with
alarms are analyzed in [ADR-0001](docs/adr/0001-full-queue-policy.md).

## Results

Ryzen 7 7840HS (Zen 4), WSL2, GCC 13.3 `-O3 -march=native`; producer and
consumer pinned to separate physical cores; queue capacity 65,536 for both queues.
These figures are for the baseline SPSC layout, before the cache-line padding and
cached-index optimizations.

**Latency**: push to pop, paced at one event per microsecond, 5M samples per run,
median of 3 runs (ns):

| Queue | p50 | p90 | p99 |
|-------|-----|-----|-----|
| `std::mutex` + `std::queue` | 542 | 2,413 | 19,754 |
| SPSC ring buffer | **114** | **243** | **5,497** |
| Improvement | 4.8x | 9.9x | 3.6x |

**Throughput**: saturating producer, median of 5 runs:

| Queue | Events/s |
|-------|----------|
| `std::mutex` + `std::queue` | 6.15 M |
| SPSC ring buffer | **24.28 M** (3.9x) |

Percentiles above p99 are dominated by WSL2 virtual-CPU descheduling and are not
comparable between queues on this machine. Methodology, ranges, and raw data:
[docs/results/baseline_vs_spsc.md](docs/results/baseline_vs_spsc.md).

## Verification

- **Unit tests** cover the API contract for both the SPSC queue and the mutex
  baseline: FIFO order, exact capacity, rejection on full, and wraparound.
- **Stress tests** push millions of checksummed, sequence-numbered events through
  queues of capacity 2, 4, and 8. Each test runs unpinned, pinned to separate
  physical cores, and pinned to SMT siblings. Any lost, duplicated, reordered, or
  torn event fails the test.
- **ThreadSanitizer and AddressSanitizer/UBSan** builds run the full suite.
- **Mutation testing** ([`scripts/mutation_test.sh`](scripts/mutation_test.sh))
  injects ordering bugs into a copy of the queue and confirms the test suite
  detects them. See [mutation results](docs/results/mutation_testing.md).

## Building

Requirements: Linux x86-64, GCC 13+ or Clang 17+, CMake 3.25+. GoogleTest is
fetched automatically.

```bash
cmake --preset release && cmake --build --preset release     # benchmarks
cmake --preset debug   && cmake --build --preset debug && ctest --preset debug
scripts/check_gates.sh        # debug + ThreadSanitizer + AddressSanitizer test runs
scripts/run_bench.sh 2 4      # latency + throughput, producer on CPU 2, consumer on CPU 4
```

## Repository layout

```
include/spsc/     spsc_queue.hpp       lock-free SPSC ring buffer
                  order_event.hpp      64-byte order event
                  backpressure.hpp     full-queue policy layer
                  wait_strategy.hpp    empty-queue wait strategies
                  mutex_queue.hpp      mutex + std::queue baseline
common/           platform helpers (TSC, CPU pinning)
tests/            unit and stress tests (GoogleTest)
bench/            latency and throughput benchmarks
examples/         order pipeline example
scripts/          gates, benchmarks, mutation testing
docs/design/      design document
docs/adr/         architecture decision records
docs/results/     benchmark and mutation-testing results
```

## Status

| Component | State |
|-----------|-------|
| SPSC queue (acquire/release, monotonic indices) | Done |
| Unit, stress, and sanitizer test suites | Done |
| Mutation testing | Done |
| Latency and throughput benchmarks vs. mutex baseline | Done |
| Full-queue backpressure policy (spin, yield, explicit reject) | Done |
| Cache-line padding and cached-index optimizations | In progress |
| Wait strategies (busy-spin, spin/yield, spin/park) | In progress |
| Bursty-load benchmark | Planned |
| End-to-end order pipeline example | Planned |
