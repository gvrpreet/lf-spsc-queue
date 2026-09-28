#pragma once

#include <cstddef>
#include <mutex>
#include <queue>

namespace spsc {

// Baseline for comparison: the "obvious" implementation. It is deliberately
// naive: std::queue (std::deque) allocates chunks on the hot path, and every
// op takes the lock. It has the same try_push/try_pop API and capacity bound
// as SpscQueue, so benches can be templated over both.
template <typename T, std::size_t Capacity>
class MutexQueue {
public:
    static constexpr std::size_t capacity() noexcept { return Capacity; }

    bool try_push(const T& value) {
        std::lock_guard lock(mutex_);
        if (queue_.size() >= Capacity) return false;
        queue_.push(value);
        return true;
    }

    bool try_pop(T& out) {
        std::lock_guard lock(mutex_);
        if (queue_.empty()) return false;
        out = queue_.front();
        queue_.pop();
        return true;
    }

    std::size_t size_approx() const {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::queue<T> queue_;
};

}  // namespace spsc
