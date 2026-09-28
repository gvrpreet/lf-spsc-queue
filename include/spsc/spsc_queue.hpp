#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace spsc {

// Bounded lock-free single-producer / single-consumer ring buffer.
//
// Contract: exactly one thread calls try_push, exactly one (other) thread calls
// try_pop. Violating this is a data race.
//
// Indices are monotonic 64-bit counters (never wrapped); slot = index & kMask.
//   empty <=> head_ == tail_          full <=> head_ - tail_ == Capacity
//
// Naming (matches the project spec):
//   head_ = next slot to WRITE, owned (written) by the producer only
//   tail_ = next slot to READ,  owned (written) by the consumer only
//
// Large capacities make this object big (65536 x 64 B = 4 MiB): allocate it
// with std::make_unique, not on the stack.
//
// See docs/design/DESIGN.md and docs/adr/0002-memory-ordering.md.
template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two >= 2");
    static_assert(std::is_trivially_copyable_v<T>,
                  "T is copied into slots by value; keep it trivially copyable");

public:
    SpscQueue() = default;
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    static constexpr std::size_t capacity() noexcept { return Capacity; }

    // Producer thread only. Returns false if the queue is full; never blocks.
    bool try_push(const T& value) noexcept {
        // relaxed: only this thread ever writes head_, so it always reads its own latest store.
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        // acquire: pairs with the consumer's release-store of tail_, so the consumer's
        // read of the slot we may be about to reuse happens-before our overwrite of it.
        // relaxed would make that slot a data race (write-after-read) under the C++
        // model even though x86 would never show it; on x86 acquire is a plain mov anyway.
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= Capacity) return false;

        buffer_[head & kMask] = value;
        // release: publishes the slot write above; a consumer that acquire-loads this
        // value is guaranteed to see the complete event, never a partial one.
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer thread only. Returns false if the queue is empty; never blocks.
    bool try_pop(T& out) noexcept {
        // relaxed: only this thread ever writes tail_.
        const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        // acquire: pairs with the producer's release-store of head_; if we see
        // head > tail, the producer's write of slot `tail` happens-before our read.
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        if (head == tail) return false;

        out = buffer_[tail & kMask];
        // release: our read of the slot must complete before the producer can observe
        // the slot as free and overwrite it (pairs with the acquire in try_push).
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Approximate occupancy for monitoring/metrics only; may be stale by the time
    // it returns. Safe to call from any thread.
    std::size_t size_approx() const noexcept {
        // acquire on tail_ first: the consumer only stored tail = t after it acquired
        // head >= t, so that head value happens-before our head_ load below. Read
        // coherence then guarantees we read head >= t, so head - tail never underflows.
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        // relaxed: the ordering we need is already established by the acquire above.
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        return static_cast<std::size_t>(head - tail);
    }

private:
    static constexpr std::uint64_t kMask = Capacity - 1;

    // Baseline layout: both indices share a cache line (ablation A0 in DESIGN.md §7).
    std::atomic<std::uint64_t> head_{0};
    std::atomic<std::uint64_t> tail_{0};
    T buffer_[Capacity];
};

}  // namespace spsc
