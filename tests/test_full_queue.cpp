// Full-queue policy test (ADR-0001). A deliberately slow consumer forces the
// queue full over and over. Whatever policy push_with_backpressure implements,
// the invariant is: every event is either delivered in order or explicitly
// counted as rejected. Nothing is silently lost.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>

#include "spsc/backpressure.hpp"
#include "spsc/cpu_relax.hpp"
#include "spsc/order_event.hpp"
#include "spsc/spsc_queue.hpp"
#include "test_util.hpp"

using spsc::OrderEvent;
namespace t = spsc::test;

namespace {
void busy_wait(std::chrono::nanoseconds d) {
    const auto until = std::chrono::steady_clock::now() + d;
    while (std::chrono::steady_clock::now() < until) spsc::cpu_relax();
}
}  // namespace

TEST(FullQueuePolicy, ThrottledConsumerNeverLosesSilently) {
    constexpr std::uint64_t kItems = 20'000;
    auto q = std::make_unique<spsc::SpscQueue<OrderEvent, 8>>();

    spsc::PushStats stats;
    std::atomic<bool> abort{false};
    std::atomic<bool> producer_done{false};
    std::atomic<std::uint64_t> progress{0};
    std::uint64_t delivered = 0;
    std::uint64_t order_violations = 0;
    std::uint64_t corrupt = 0;

    auto producer = [&] {
        for (std::uint64_t seq = 0; seq < kItems && !abort.load(std::memory_order_relaxed); ++seq) {
            spsc::push_with_backpressure(*q, t::make_event(seq), stats);
            progress.fetch_add(1, std::memory_order_relaxed);
        }
        producer_done.store(true, std::memory_order_release);
    };
    auto consumer = [&] {
        OrderEvent out{};
        std::uint64_t last_seq = 0;
        bool first = true;
        auto handle = [&] {
            if (out.checksum != t::checksum_of(out)) ++corrupt;
            if (!first && out.seq <= last_seq) ++order_violations;
            first = false;
            last_seq = out.seq;
            ++delivered;
            busy_wait(std::chrono::microseconds(2));  // artificially slow matcher
        };
        while (!abort.load(std::memory_order_relaxed)) {
            if (q->try_pop(out)) {
                handle();
            } else if (producer_done.load(std::memory_order_acquire)) {
                while (q->try_pop(out)) handle();  // drain what's left
                return;
            } else {
                spsc::cpu_relax();
            }
        }
    };

    ASSERT_TRUE(t::run_producer_consumer(producer, consumer, abort, progress, std::chrono::seconds(10)));
    EXPECT_EQ(delivered + stats.rejected, kItems) << "events lost silently";
    EXPECT_EQ(stats.pushed, delivered);
    EXPECT_EQ(order_violations, 0u);
    EXPECT_EQ(corrupt, 0u);
    EXPECT_GT(stats.full_events, 0u) << "consumer throttle should have filled the queue";
}
