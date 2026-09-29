#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace spsc {

// Compile-time tuning knobs. The defaults are the recommended configuration; the
// other settings exist so each optimization can be measured in isolation (see the
// performance experiments in docs/design/DESIGN.md §7).
struct Tuning {
    // Alignment of each thread's index group. 64 places head_ and tail_ on separate
    // cache lines; 128 also separates adjacent-line prefetch pairs; 8 (alignof the
    // index) packs both indices into one line, i.e. deliberate false sharing.
    std::size_t index_align = 64;
    // Each thread keeps a private copy of the other thread's index and re-reads the
    // shared one only when the copy says the queue is full (producer) / empty (consumer).
    bool cache_remote_index = true;
    // Consumer prefetches the slot after the one it just read.
    bool prefetch_next = false;
    // Use seq_cst for every atomic operation. Comparison point only: acquire/release
    // is sufficient (docs/adr/0002-memory-ordering.md).
    bool seq_cst = false;
};

inline constexpr Tuning kBaselineTuning{.index_align = 8, .cache_remote_index = false};

// Bounded lock-free single-producer / single-consumer ring buffer.
//
// Contract: exactly one thread calls try_push, exactly one (other) thread calls
// try_pop. Violating this is a data race.
//
// Indices are monotonic 64-bit counters (never wrapped); slot = index & kMask.
//   empty <=> head_ == tail_          full <=> head_ - tail_ == Capacity
//
//   head_ = next slot to WRITE, owned (written) by the producer only
//   tail_ = next slot to READ,  owned (written) by the consumer only
//
// Large capacities make this object big (65536 x 64 B = 4 MiB): allocate it
// with std::make_unique, not on the stack.
//
// See docs/design/DESIGN.md and docs/adr/0002-memory-ordering.md.
template <typename T, std::size_t Capacity, Tuning Tune = Tuning{}>
class SpscQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two >= 2");
    static_assert(std::is_trivially_copyable_v<T>,
                  "T is copied into slots by value; keep it trivially copyable");
    static_assert(Tune.index_align >= alignof(std::atomic<std::uint64_t>) &&
                      (Tune.index_align & (Tune.index_align - 1)) == 0,
                  "index_align must be a power of two >= alignof(std::atomic<uint64_t>)");

    static constexpr std::memory_order kOwn =
        Tune.seq_cst ? std::memory_order_seq_cst : std::memory_order_relaxed;
    static constexpr std::memory_order kAcquire =
        Tune.seq_cst ? std::memory_order_seq_cst : std::memory_order_acquire;
    static constexpr std::memory_order kRelease =
        Tune.seq_cst ? std::memory_order_seq_cst : std::memory_order_release;

public:
    SpscQueue() = default;
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    static constexpr std::size_t capacity() noexcept { return Capacity; }

    // Producer thread only. Returns false if the queue is full; never blocks.
    bool try_push(const T& value) noexcept {
        // relaxed (kOwn): only this thread ever writes head_, so it always reads its
        // own latest store.
        const std::uint64_t head = head_.load(kOwn);
        if constexpr (Tune.cache_remote_index) {
            // The cached tail is a value we acquired earlier, so every slot it marks as
            // free was already read by the consumer; we only touch the shared line when
            // the cache says "full".
            if (head - cached_tail_ >= Capacity) {
                // acquire: pairs with the consumer's release-store of tail_, so its read
                // of the slot we are about to reuse happens-before our overwrite of it.
                cached_tail_ = tail_.load(kAcquire);
                if (head - cached_tail_ >= Capacity) return false;
            }
        } else {
            // acquire: pairs with the consumer's release-store of tail_, so the consumer's
            // read of the slot we may be about to reuse happens-before our overwrite of it.
            // relaxed would make that slot a data race (write-after-read) under the C++
            // model even though x86 would never show it; on x86 acquire is a plain mov anyway.
            const std::uint64_t tail = tail_.load(kAcquire);
            if (head - tail >= Capacity) return false;
        }

        buffer_[head & kMask] = value;
        // release: publishes the slot write above; a consumer that acquire-loads this
        // value is guaranteed to see the complete event, never a partial one.
        head_.store(head + 1, kRelease);
        return true;
    }

    // Consumer thread only. Returns false if the queue is empty; never blocks.
    bool try_pop(T& out) noexcept {
        // relaxed (kOwn): only this thread ever writes tail_.
        const std::uint64_t tail = tail_.load(kOwn);
        if constexpr (Tune.cache_remote_index) {
            if (tail == cached_head_) {
                // acquire: pairs with the producer's release-store of head_; every slot
                // below the value we read is fully written and visible to us.
                cached_head_ = head_.load(kAcquire);
                if (tail == cached_head_) return false;
            }
        } else {
            // acquire: pairs with the producer's release-store of head_; if we see
            // head > tail, the producer's write of slot `tail` happens-before our read.
            const std::uint64_t head = head_.load(kAcquire);
            if (head == tail) return false;
        }

        out = buffer_[tail & kMask];
        if constexpr (Tune.prefetch_next) {
            __builtin_prefetch(&buffer_[(tail + 1) & kMask], 0, 3);
        }
        // release: our read of the slot must complete before the producer can observe
        // the slot as free and overwrite it (pairs with the acquire in try_push).
        tail_.store(tail + 1, kRelease);
        return true;
    }

    // Approximate occupancy for monitoring/metrics only; may be stale by the time
    // it returns. Safe to call from any thread.
    std::size_t size_approx() const noexcept {
        // acquire on tail_ first: the consumer only stored tail = t after it acquired
        // head >= t, so that head value happens-before our head_ load below. Read
        // coherence then guarantees we read head >= t, so head - tail never underflows.
        const std::uint64_t tail = tail_.load(kAcquire);
        // relaxed: the ordering we need is already established by the acquire above.
        const std::uint64_t head = head_.load(kOwn);
        return static_cast<std::size_t>(head - tail);
    }

private:
    static constexpr std::uint64_t kMask = Capacity - 1;
    static constexpr std::size_t kBufferAlign = std::max<std::size_t>(Tune.index_align, alignof(T));

    // Producer-owned line: the index it publishes plus its private copy of tail_.
    alignas(Tune.index_align) std::atomic<std::uint64_t> head_{0};
    std::uint64_t cached_tail_ = 0;
    // Consumer-owned line: the index it publishes plus its private copy of head_.
    alignas(Tune.index_align) std::atomic<std::uint64_t> tail_{0};
    std::uint64_t cached_head_ = 0;
    // The buffer starts on its own line so slot 0 never shares a line with tail_.
    alignas(kBufferAlign) T buffer_[Capacity];
};

}  // namespace spsc
