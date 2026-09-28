#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

#include "spsc/cpu_relax.hpp"

namespace spsc {

// Full-queue policy (docs/adr/0001-full-queue-policy.md). The core queue never
// blocks or drops; this layer decides what the producer does when try_push fails.
//
// Invariant: an event is either delivered to the queue or counted in `rejected`
// (for the caller to reject back to the client and alert on). Never silently lost.
struct PushStats {
    std::uint64_t pushed = 0;
    std::uint64_t full_events = 0;  // pushes that found the queue full at least once
    std::uint64_t spins = 0;
    std::uint64_t yields = 0;
    std::uint64_t rejected = 0;     // explicit rejects after the time budget expired
};

struct BackpressureConfig {
    // pause-spins before starting to yield: covers short consumer hiccups at
    // the lowest possible hand-off latency.
    std::uint32_t spin_limit = 1024;
    // Time spent yielding before the event is rejected explicitly. Zero means
    // never reject (pure blocking backpressure).
    std::chrono::nanoseconds reject_after = std::chrono::milliseconds(1);
};

// Returns true if delivered, false if explicitly rejected (stats.rejected is incremented).
// The fast path is a single try_push with no timing or extra shared-memory traffic.
template <typename Queue, typename T>
bool push_with_backpressure(Queue& q, const T& value, PushStats& stats,
                            const BackpressureConfig& cfg = {}) {
    if (q.try_push(value)) [[likely]] {
        ++stats.pushed;
        return true;
    }

    ++stats.full_events;
    for (std::uint32_t i = 0; i < cfg.spin_limit; ++i) {
        ++stats.spins;
        cpu_relax();
        if (q.try_push(value)) {
            ++stats.pushed;
            return true;
        }
    }

    using clock = std::chrono::steady_clock;
    const auto yield_start = clock::now();
    for (;;) {
        ++stats.yields;
        std::this_thread::yield();
        if (q.try_push(value)) {
            ++stats.pushed;
            return true;
        }
        if (cfg.reject_after.count() > 0 && clock::now() - yield_start >= cfg.reject_after) {
            ++stats.rejected;
            return false;
        }
    }
}

}  // namespace spsc
