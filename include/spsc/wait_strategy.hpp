#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "spsc/cpu_relax.hpp"

namespace spsc {

// Empty-queue wait strategies (docs/adr/0003-wait-strategy.md).
//
// Every strategy exposes the same interface:
//   consumer:  wait.pop(queue, out);   // blocks until an element is popped
//   producer:  queue.try_push(ev); wait.notify();
//   shutdown:  wait.wake_all();        // releases a consumer parked in pop()
// Only SpinThenPark does real work in notify(); for the others it compiles away.

// Lowest wake-up latency; burns 100% of a core while idle.
struct BusySpin {
    template <typename Queue, typename T>
    void pop(Queue& q, T& out) noexcept {
        while (!q.try_pop(out)) cpu_relax();
    }
    void notify() noexcept {}
    void wake_all() noexcept {}
};

// Spins briefly, then yields the CPU between attempts. Still ~100% CPU when no
// other thread wants the core, but gives way when one does.
class SpinThenYield {
public:
    explicit SpinThenYield(std::uint32_t spin_limit = 1024) : spin_limit_(spin_limit) {}

    template <typename Queue, typename T>
    void pop(Queue& q, T& out) {
        for (std::uint32_t spins = 0; !q.try_pop(out);) {
            if (spins < spin_limit_) {
                ++spins;
                cpu_relax();
            } else {
                std::this_thread::yield();
            }
        }
    }
    void notify() noexcept {}
    void wake_all() noexcept {}

private:
    std::uint32_t spin_limit_;
};

// Spins briefly, then sleeps in the kernel (futex via std::atomic::wait) until the
// producer notifies. Near-zero idle CPU; microseconds of wake-up latency once parked.
//
// Lost-wakeup protocol (Dekker-style, two variables):
//   consumer: sleeping_ = true;  seq_cst fence;  re-check queue (loads head_)
//   producer: publish head_;     seq_cst fence;  load sleeping_
// With a seq_cst fence between each thread's store and load, at least one side is
// guaranteed to observe the other's store: either the consumer's re-check finds the
// element, or the producer sees sleeping_ and notifies. Release/acquire alone does
// not order a store before a later load, so both could miss (a lost wake-up).
// The wait itself is on a separate epoch counter: the consumer reads it *before*
// announcing sleep, so a notify that lands between the re-check and wait() changes
// the epoch and makes wait() return immediately.
//
// Cost: the producer executes a seq_cst fence (mfence on x86) on every notify().
class SpinThenPark {
public:
    explicit SpinThenPark(std::uint32_t spin_limit = 1024) : spin_limit_(spin_limit) {}

    template <typename Queue, typename T>
    void pop(Queue& q, T& out) {
        for (;;) {
            for (std::uint32_t spins = 0; spins < spin_limit_; ++spins) {
                if (q.try_pop(out)) return;
                cpu_relax();
            }
            // acquire: pairs with the producer's release increment, so any element
            // published before that notify is visible to the re-check below.
            const std::uint32_t epoch = epoch_.load(std::memory_order_acquire);
            sleeping_.store(true, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (q.try_pop(out)) {
                sleeping_.store(false, std::memory_order_relaxed);
                return;
            }
            // Sleeps only while epoch_ still equals `epoch`.
            parks_.fetch_add(1, std::memory_order_relaxed);
            epoch_.wait(epoch, std::memory_order_acquire);
            sleeping_.store(false, std::memory_order_relaxed);
        }
    }

    // Producer: call after every successful push.
    void notify() noexcept {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (sleeping_.load(std::memory_order_relaxed)) wake_all();
    }

    void wake_all() noexcept {
        // release: the element published before this call is visible to the
        // consumer's acquire load of epoch_.
        epoch_.fetch_add(1, std::memory_order_release);
        epoch_.notify_one();
    }

    // Number of times the consumer attempted to park (for diagnostics).
    std::uint64_t parks() const noexcept { return parks_.load(std::memory_order_relaxed); }

private:
    std::uint32_t spin_limit_;
    // Written by the consumer only when parking; read by the producer on every notify.
    alignas(64) std::atomic<bool> sleeping_{false};
    std::atomic<std::uint64_t> parks_{0};
    // Written by the producer only when the consumer is (or may be) asleep.
    alignas(64) std::atomic<std::uint32_t> epoch_{0};
};

}  // namespace spsc
