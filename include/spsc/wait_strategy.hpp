#pragma once

#include <cstdint>
#include <thread>

#include "spsc/cpu_relax.hpp"

namespace spsc {

// Empty-queue wait strategies (docs/adr/0003-wait-strategy.md).
//
// Intended consumer usage:
//     while (!q.try_pop(ev)) wait.idle();
//     wait.reset();
// Intended producer usage (only SpinThenPark needs it):
//     q.try_push(ev); wait.notify();

struct BusySpin {
    void idle() noexcept { cpu_relax(); }
    void reset() noexcept {}
    void notify() noexcept {}
};

class SpinThenYield {
public:
    explicit SpinThenYield(std::uint32_t spin_limit = 1000) : spin_limit_(spin_limit) {}

    void idle() noexcept {
        if (spins_ < spin_limit_) {
            ++spins_;
            cpu_relax();
        } else {
            std::this_thread::yield();
        }
    }
    void reset() noexcept { spins_ = 0; }
    void notify() noexcept {}

private:
    std::uint32_t spin_limit_;
    std::uint32_t spins_ = 0;
};

// TODO: SpinThenPark: spin, then park via std::atomic<T>::wait (futex on Linux).
// The producer notifies only when the consumer may be asleep; see ADR-0003 for
// the protocol and why its flag/index accesses require seq_cst.

}  // namespace spsc
