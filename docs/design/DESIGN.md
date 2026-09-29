# Design: SPSC Ring Buffer for Order Intake and Matching

## 1. Thread model

```
 Intake thread (producer)            SPSC ring (bounded, 2^k slots)            Matching thread (consumer)
+-------------------------+        +---------------------------------+        +---------------------------+
| parse request           |  push  | [slot][slot][slot] ... [slot]   |  pop   | apply to order book       |
| validate                | -----> |   tail_: consumer-owned         | -----> | generate trades / acks    |
| build OrderEvent        |        |   head_: producer-owned         |        |                           |
+-------------------------+        +---------------------------------+        +---------------------------+
       pinned core A                                                                  pinned core B
```

- Exactly one producer thread and one consumer thread. This is an API contract;
  the type system does not enforce it, and violating it is a data race.
- The order book is owned exclusively by the consumer thread, so it needs no locks.
- Acknowledgements and trades flowing back to the gateway would use a second SPSC
  queue in the opposite direction. That is out of scope for this component.

## 2. Data layout

```cpp
template <typename T, std::size_t Capacity, Tuning Tune = Tuning{}>  // Capacity = 2^k
class SpscQueue {
    alignas(64) std::atomic<uint64_t> head_;  // next slot to write; written by the producer only
    uint64_t cached_tail_;                     // producer's private copy of tail_
    alignas(64) std::atomic<uint64_t> tail_;  // next slot to read; written by the consumer only
    uint64_t cached_head_;                     // consumer's private copy of head_
    alignas(64) T buffer_[Capacity];          // T = OrderEvent, 64 bytes, stored by value
};
```

`Tuning` is a compile-time parameter (a C++20 class-type template argument) that
switches each optimization on or off, so every variant is the same code, tested by
the same suite and compared in the same benchmark binary. `if constexpr` removes
disabled paths, so the knobs cost nothing at runtime. `kBaselineTuning` recreates
the original unpadded, uncached layout.

- **Naming.** `head_` is the write position (producer) and `tail_` is the read
  position (consumer). Some libraries use the opposite convention.
- **Slots by value.** Events are copied into the ring rather than referenced by
  pointer: there is no allocation, deallocation, or pointer-chasing on the hot
  path, and a hand-off is a single 64-byte copy.
- **One slot per cache line.** `OrderEvent` is exactly 64 bytes and `alignas(64)`,
  so consecutive slots never straddle a line. On Zen 4 with AVX-512 the whole
  slot is copied with one aligned `vmovdqa64` load and one store.
- **Index placement.** Each thread's hot state is exactly one cache line: the
  index it publishes plus its cached copy of the other index. The baseline layout,
  with both indices on one line, suffers false sharing: every push invalidates
  the consumer's copy of the line, and every pop invalidates the producer's.
- **Cached remote index.** The producer consults its private `cached_tail_` and
  re-reads the shared `tail_` only when the cache says the queue is full. The
  consumer does the same with `head_`. A stale cache can only make the queue look
  fuller (producer) or emptier (consumer) than it really is, never the reverse,
  so staleness costs a retry but never correctness. Every cached value was itself
  obtained by an acquire load.

## 3. Indices: monotonic counters with a power-of-two mask

- `head_` and `tail_` are monotonically increasing `uint64_t` counters that are
  never wrapped. The slot for an index is `index & (Capacity - 1)`.
- `size = head - tail`. Unsigned arithmetic makes this correct even across
  2^64 overflow, which at 10^9 operations per second would take about 584 years.
- `empty <=> head == tail` and `full <=> head - tail == Capacity`. All
  `Capacity` slots are usable. The alternative, wrapped indices, needs one
  sacrificed slot (or an extra flag) to tell full from empty.
- A power-of-two capacity turns the modulo into a single `and`. A general `% N`
  compiles to an integer division costing tens of cycles.

## 4. Memory ordering

| Operation | Thread | Order | Reason |
|-----------|--------|-------|--------|
| load `head_` in `try_push` | producer | relaxed | Only this thread writes `head_`, so it always observes its own latest store. |
| load `tail_` in `try_push` | producer | acquire | Pairs with the consumer's release store of `tail_`, so the consumer's read of a slot happens-before the producer's overwrite of it. |
| write slot | producer | plain store | Ordered before the publish by the release store below. |
| store `head_` | producer | release | Publishes the slot contents to any thread that acquire-loads this value. |
| load `tail_` in `try_pop` | consumer | relaxed | Only this thread writes `tail_`. |
| load `head_` in `try_pop` | consumer | acquire | Pairs with the producer's release; the slot contents are visible afterwards. |
| read slot | consumer | plain load | Ordered after the acquire. |
| store `tail_` | consumer | release | Frees the slot only after the read of it has completed. |

**Publishing (producer to consumer).** The slot write is sequenced before the
release store of `head_`. The consumer's acquire load reads that value, so the
store synchronizes-with the load. The load is sequenced before the slot read.
Therefore the write happens-before the read, and the two do not race.

**Reclaiming (consumer to producer).** The same argument runs in reverse: the
consumer's slot read is sequenced before its release store of `tail_`, which
synchronizes-with the producer's acquire load of `tail_`, which is sequenced
before the producer's next write to that slot.

**Acquire versus relaxed for the other side's index.** A relaxed load of `head_`
in `try_pop` is clearly wrong: the consumer could read a slot before its contents
are visible. The producer's load of `tail_` is subtler. With a relaxed load, no
happens-before edge connects the consumer's read of slot *i* to the producer's
subsequent write of slot *i*. Those are conflicting non-atomic accesses, so by
definition they form a data race, which is undefined behavior. In practice
the race is not observable on mainstream hardware:
- x86-TSO never reorders a load with a later store.
- On ARM and POWER, the store is control-dependent on the load, and stores are
  not speculated.

The C++ memory model, however, does not recognize control dependencies. The
compiler is entitled to assume the program is race-free. The acquire is also
free on x86, where it compiles to the same plain `mov` as a relaxed load (see
[ADR-0002](../adr/0002-memory-ordering.md)). So the queue uses acquire.

## 5. Full-queue and empty-queue policies

The core queue is policy-free: `try_push` and `try_pop` never block and never
drop. Policies are layered on top:
- **Full queue:** [ADR-0001](../adr/0001-full-queue-policy.md), implemented in
  `include/spsc/backpressure.hpp`. An order is never dropped silently.
- **Empty queue:** [ADR-0003](../adr/0003-wait-strategy.md), implemented in
  `include/spsc/wait_strategy.hpp`.

## 6. Why SPSC has no ABA problem

ABA occurs when a thread reads value A, is delayed while the value changes
A -> B -> A, and then a compare-and-swap succeeds even though the state it
guards has changed. The classic case is popping from a Treiber stack with node reuse.

ABA needs two ingredients: a CAS that decides based on a previously read value,
and reuse of that value in the meantime. This queue has neither:
- **No CAS.** Each index has exactly one writer, so there is no write-write
  contention to arbitrate. The producer does not compete for a slot; it owns the
  next one.
- **No index reuse.** The indices are 64-bit monotonic counters, so a value is
  never observed twice.

Slots *are* reused every `Capacity` operations. That reuse is gated by the
happens-before edge from Section 4: the producer can only overwrite a slot after
acquiring a `tail_` value that has moved past it.

With two producers, both could read the same `head_` and write the same slot.
Claiming a slot then requires a CAS on `head_`. Publishing it requires a
per-slot sequence number, because claiming and completing the write are no
longer a single step. This is the design of Vyukov's bounded MPMC queue, and it
is considerably more complex and more expensive.

## 7. Performance experiments

Each change is applied and measured in isolation. Results and analysis:
[docs/results/ablations.md](../results/ablations.md). In summary, the tuned layout
lowers hand-off latency by about 9% at p50 and 15% at p90. Saturated throughput is
limited by the consumer repeatedly catching up with the producer and re-reading its
index, not by false sharing.

| ID | Change | Hypothesis |
|----|--------|------------|
| A0 | Baseline: indices on the same cache line | Reference point |
| A1 | `head_` and `tail_` on separate cache lines | Removes false sharing; large throughput gain |
| A2 | Cached remote index (the producer caches `tail_`, the consumer caches `head_`) | The remote line is read once per ~Capacity operations instead of every operation |
| A3 | 64-byte aligned events versus a 48-byte unaligned event | Aligned slots never straddle two lines |
| A4 | Software prefetch of the next slot | Likely neutral; the hardware prefetcher already handles sequential access |
| A5 | `seq_cst` throughout versus acquire/release | On x86, a seq_cst store becomes `xchg`, which drains the store buffer |
| A6 | Producer and consumer on SMT siblings versus separate cores | Shared L1/L2 lowers hand-off latency |
| A7 | Batched pop / batched index publication | Fewer index writes raise throughput at some cost in latency |

## 8. Testing strategy

| Failure mode | Detection |
|--------------|-----------|
| Consumer reads a slot before it is written | `test_wraparound_stress` (sequence number + checksum), ThreadSanitizer |
| Producer overwrites a slot before it is read | `test_wraparound_stress` with capacities 2-8 |
| Order lost when the queue is full | `test_full_queue` (throttled consumer; delivered + rejected == sent) |
| Lost wake-up in the park protocol | `test_wait_strategy` ping-pong: one event in flight, randomized gaps; a lost wake-up is a detected stall |
| Order book corrupted by the matcher | `test_order_book` (price-time priority, randomized invariant checks); the pipeline example re-checks invariants at shutdown |
| Test suite unable to detect ordering bugs | `scripts/mutation_test.sh` injects 15 ordering and protocol bugs and checks that each is caught |

The stress tests run unpinned, pinned to separate physical cores, and pinned to
SMT siblings. Tiny capacities force constant full/empty transitions and millions
of wraparounds.

## 9. Future work

- Return-path queue (matcher to gateway).
- Sharding by instrument: N SPSC queues, one per matching thread.
- Production deployment of busy-spin consumers requires isolated cores
  (`isolcpus`, `nohz_full`).
