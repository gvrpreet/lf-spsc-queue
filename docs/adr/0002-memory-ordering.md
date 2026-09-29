# ADR-0002: Memory ordering for head and tail indices

- **Status:** Accepted
- **Date:** 2026-09-28

## Context

Two threads communicate through non-atomic slots guarded by two atomic indices,
each with exactly one writer. The goal is the weakest ordering that is correct
under the C++ memory model, not merely on x86, because:

1. Code that is correct only on x86 contains data races under the C++ model.
   Those races are undefined behavior, and the compiler may optimize on the assumption that they do not occur.
2. Weaker orderings can be cheaper. On x86, a `seq_cst` store compiles to `xchg`
   (or `mov` + `mfence`), which drains the store buffer.

## Options considered

1. **`seq_cst` everywhere.** Simplest to reason about, but costs a full barrier on every store.
2. **Release/acquire on cross-thread index accesses, relaxed on each thread's own index.**
3. **Relaxed load of the other side's index,** relying on hardware ordering or
   control dependencies. The C++ model does not recognize control dependencies,
   so this is formally a data race on slot reuse.

## Decision

Option 2:

- Each thread loads its own index with `relaxed`.
- Each thread loads the other thread's index with `acquire`.
- Each thread publishes its own index with `release`.
- `size_approx()` loads `tail_` with `acquire` before loading `head_`. This
  guarantees `head >= tail`, so the subtraction cannot underflow.

The full happens-before argument is in [DESIGN.md, section 4](../design/DESIGN.md#4-memory-ordering).

## Evidence

- GCC 13.3 at `-O3 -march=native` compiles every acquire load and release store
  in `try_push`/`try_pop` to a plain `mov`, with no fences. On x86 the chosen
  orderings therefore constrain only the compiler, not the hardware.
- The stress and unit tests pass under ThreadSanitizer.
- `scripts/mutation_test.sh` weakens each ordering in turn to check whether the
  test suite detects the change. Results are in
  [docs/results/mutation_testing.md](../results/mutation_testing.md).

## Consequences

- The code is correct on weakly ordered architectures (ARM, POWER) without modification.
- There is no runtime cost on x86 compared with relaxed loads.
- Ablation A5 ([results](../results/ablations.md)) compared `seq_cst` everywhere.
  Hand-off latency was indistinguishable (p50 103 vs. 98 ns). Saturated throughput
  *doubled*, but not because `seq_cst` is cheaper. The locked `xchg` on the
  consumer's `tail_` store slowed the consumer enough that it stopped repeatedly
  catching up with the producer and re-reading its index. The finding argues for
  consumer back-off or batching when the queue is empty. It does not argue for
  stronger orderings, so this decision stands.
