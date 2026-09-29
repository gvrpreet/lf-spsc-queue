// Wait-strategy tests (ADR-0003).
//
// PingPongNoLostWakeup: the producer sends one event at a time and waits for the
// consumer to acknowledge it before sending the next, with randomized gaps before
// each push: none (the consumer is still spinning), short (it is about to park), or
// long (it is parked in the kernel). Because only one event is ever in flight, a
// lost wake-up cannot be rescued by a later push; it shows up as a stall and fails.
//
// Env: SPSC_WAIT_ITEMS (ping-pong round trips, default 20000)
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <random>
#include <thread>

#include "spsc/cpu_relax.hpp"
#include "spsc/order_event.hpp"
#include "spsc/spsc_queue.hpp"
#include "spsc/wait_strategy.hpp"
#include "test_util.hpp"

using spsc::OrderEvent;
namespace t = spsc::test;

namespace {

constexpr std::uint64_t kPoison = ~0ull;

void busy_wait(std::chrono::nanoseconds d) {
    const auto until = std::chrono::steady_clock::now() + d;
    while (std::chrono::steady_clock::now() < until) spsc::cpu_relax();
}

}  // namespace

template <typename Wait>
class WaitStrategyTest : public ::testing::Test {
protected:
    // Small spin budgets so the consumer parks/yields often. Constructed in place:
    // SpinThenPark holds atomics and is not movable.
    static std::unique_ptr<Wait> make() {
        if constexpr (std::is_same_v<Wait, spsc::BusySpin>) {
            return std::make_unique<Wait>();
        } else {
            return std::make_unique<Wait>(16u);
        }
    }
};

using Strategies = ::testing::Types<spsc::BusySpin, spsc::SpinThenYield, spsc::SpinThenPark>;
TYPED_TEST_SUITE(WaitStrategyTest, Strategies);

TYPED_TEST(WaitStrategyTest, PingPongNoLostWakeup) {
    const std::uint64_t n = t::env_u64("SPSC_WAIT_ITEMS", 20'000);
    auto q = std::make_unique<spsc::SpscQueue<OrderEvent, 8>>();
    auto wait = TestFixture::make();

    std::atomic<bool> abort{false};
    std::atomic<std::uint64_t> acked{0};
    std::uint64_t bad = 0;

    auto producer = [&] {
        std::mt19937 rng(12345);
        std::uniform_int_distribution<int> kind(0, 3);
        std::uniform_int_distribution<int> short_ns(0, 2'000);
        for (std::uint64_t seq = 0; seq < n; ++seq) {
            switch (kind(rng)) {
                case 0: break;                                                          // immediate
                case 1: busy_wait(std::chrono::nanoseconds(short_ns(rng))); break;      // racing the park
                case 2: busy_wait(std::chrono::microseconds(20)); break;                // likely parked
                default: std::this_thread::sleep_for(std::chrono::microseconds(50)); break;
            }
            // At most one event is in flight, so this succeeds on the first attempt.
            while (!q->try_push(t::make_event(seq))) spsc::cpu_relax();
            wait->notify();
            while (acked.load(std::memory_order_acquire) != seq + 1) {
                if (abort.load(std::memory_order_relaxed)) {
                    // Watchdog fired: release a consumer stuck in pop() so the test can end.
                    OrderEvent poison = t::make_event(0);
                    poison.seq = kPoison;
                    while (!q->try_push(poison)) spsc::cpu_relax();
                    wait->wake_all();
                    return;
                }
                spsc::cpu_relax();
            }
        }
    };
    auto consumer = [&] {
        OrderEvent out{};
        for (std::uint64_t expected = 0; expected < n; ++expected) {
            wait->pop(*q, out);
            if (out.seq == kPoison) return;
            if (!t::is_valid(out, expected)) ++bad;
            acked.store(expected + 1, std::memory_order_release);
        }
    };

    const bool completed = t::run_producer_consumer(producer, consumer, abort, acked);
    ASSERT_TRUE(completed) << "stalled at " << acked.load() << "/" << n
                           << " (lost wake-up or deadlock)";
    EXPECT_EQ(bad, 0u);
    if constexpr (std::is_same_v<TypeParam, spsc::SpinThenPark>) {
        EXPECT_GT(wait->parks(), 0u) << "test never exercised the park path";
    }
}

TYPED_TEST(WaitStrategyTest, StreamPreservesOrder) {
    const std::uint64_t n = t::env_u64("SPSC_STRESS_ITEMS", 5'000'000) / 5;
    auto q = std::make_unique<spsc::SpscQueue<OrderEvent, 64>>();
    auto wait = TestFixture::make();

    std::atomic<bool> abort{false};
    std::atomic<std::uint64_t> consumed{0};
    std::uint64_t bad = 0;

    auto producer = [&] {
        for (std::uint64_t seq = 0; seq < n; ++seq) {
            const OrderEvent ev = t::make_event(seq);
            while (!q->try_push(ev)) {
                if (abort.load(std::memory_order_relaxed)) return;
                spsc::cpu_relax();
            }
            wait->notify();
            if ((seq & 0xFFFF) == 0) std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        // The consumer may be parked on a missed wake-up; wait for it or for the watchdog.
        while (consumed.load(std::memory_order_relaxed) != n) {
            if (abort.load(std::memory_order_relaxed)) {
                OrderEvent poison = t::make_event(0);
                poison.seq = kPoison;
                while (!q->try_push(poison)) spsc::cpu_relax();
                wait->wake_all();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    auto consumer = [&] {
        OrderEvent out{};
        for (std::uint64_t expected = 0; expected < n; ++expected) {
            wait->pop(*q, out);
            if (out.seq == kPoison) return;
            if (!t::is_valid(out, expected)) ++bad;
            consumed.store(expected + 1, std::memory_order_relaxed);
        }
    };

    ASSERT_TRUE(t::run_producer_consumer(producer, consumer, abort, consumed));
    EXPECT_EQ(bad, 0u);
}
