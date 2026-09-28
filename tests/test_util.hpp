#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>

#include "spsc/order_event.hpp"

namespace spsc::test {

inline std::uint32_t checksum_of(const OrderEvent& e) {
    std::uint64_t h = 0x9E3779B97F4A7C15ull;
    auto mix = [&h](std::uint64_t v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); };
    mix(e.seq);
    mix(e.order_id);
    mix(e.client_id);
    mix(static_cast<std::uint64_t>(e.price_ticks));
    mix(e.qty);
    mix(e.instrument_id);
    mix(static_cast<std::uint8_t>(e.type));
    mix(static_cast<std::uint8_t>(e.side));
    return static_cast<std::uint32_t>(h ^ (h >> 32));
}

// Deterministic event derived from `seq`, so the consumer can verify every field.
inline OrderEvent make_event(std::uint64_t seq) {
    OrderEvent e{};
    e.seq = seq;
    e.order_id = seq * 7 + 1;
    e.client_id = seq % 97;
    e.price_ticks = 10'000 + static_cast<std::int64_t>(seq % 200) - 100;
    e.qty = static_cast<std::uint32_t>(seq % 1000 + 1);
    e.instrument_id = static_cast<std::uint32_t>(seq % 16);
    e.type = static_cast<EventType>(seq % 3);
    e.side = static_cast<Side>(seq % 2);
    e.checksum = checksum_of(e);
    return e;
}

inline bool is_valid(const OrderEvent& e, std::uint64_t expected_seq) {
    return e.seq == expected_seq && e.checksum == checksum_of(e) &&
           e.order_id == expected_seq * 7 + 1;
}

inline std::uint64_t env_u64(const char* name, std::uint64_t fallback) {
    const char* v = std::getenv(name);
    return v ? std::stoull(v) : fallback;
}

// Runs producer and consumer on two threads. If `progress` stops advancing
// for `stall_timeout`, sets `abort` (both loops must poll it) and returns
// false, so a broken or unimplemented queue fails fast instead of hanging.
template <typename Producer, typename Consumer>
bool run_producer_consumer(Producer&& producer, Consumer&& consumer, std::atomic<bool>& abort,
                           const std::atomic<std::uint64_t>& progress,
                           std::chrono::milliseconds stall_timeout = std::chrono::seconds(2)) {
    using clock = std::chrono::steady_clock;
    std::atomic<int> done{0};
    std::jthread tp([&] { producer(); done.fetch_add(1); });
    std::jthread tc([&] { consumer(); done.fetch_add(1); });

    auto last_value = progress.load(std::memory_order_relaxed);
    auto last_change = clock::now();
    while (done.load() < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto v = progress.load(std::memory_order_relaxed);
        if (v != last_value) {
            last_value = v;
            last_change = clock::now();
        } else if (clock::now() - last_change > stall_timeout) {
            abort.store(true);
            return false;  // jthreads join on scope exit
        }
    }
    return true;
}

}  // namespace spsc::test
