// Two-thread stress test with tiny capacities to force constant full/empty
// collisions and millions of wraparounds. Every event carries a sequence number
// and checksum, so torn, early, lost, duplicated, or reordered reads are caught.
//
// Env:
//   SPSC_STRESS_ITEMS    items per test (default 5M; the tsan preset sets it lower)
//   SPSC_PRODUCER_CPU    CPU for the pinned variant's producer (default 2)
//   SPSC_CONSUMER_CPU    CPU for the pinned variant's consumer (default 4)
//   SPSC_SMT_SIBLING_CPU consumer CPU for the SMT variant (default 3, sibling of 2)
#include <gtest/gtest.h>

#include <atomic>
#include <memory>

#include "platform.hpp"
#include "spsc/cpu_relax.hpp"
#include "spsc/mutex_queue.hpp"
#include "spsc/order_event.hpp"
#include "spsc/spsc_queue.hpp"
#include "test_util.hpp"

using spsc::OrderEvent;
namespace t = spsc::test;

namespace {

template <typename Queue>
void run_stress(int producer_cpu, int consumer_cpu) {
    const std::uint64_t n = t::env_u64("SPSC_STRESS_ITEMS", 5'000'000);
    auto q = std::make_unique<Queue>();

    std::atomic<bool> abort{false};
    std::atomic<std::uint64_t> consumed{0};
    std::uint64_t bad_count = 0;
    std::uint64_t first_bad_seq = ~0ull;

    auto producer = [&] {
        platform::pin_current_thread(producer_cpu);
        for (std::uint64_t seq = 0; seq < n; ++seq) {
            const OrderEvent ev = t::make_event(seq);
            while (!q->try_push(ev)) {
                if (abort.load(std::memory_order_relaxed)) return;
                spsc::cpu_relax();
            }
        }
    };
    auto consumer = [&] {
        platform::pin_current_thread(consumer_cpu);
        OrderEvent out{};
        for (std::uint64_t expected = 0; expected < n; ++expected) {
            while (!q->try_pop(out)) {
                if (abort.load(std::memory_order_relaxed)) return;
                spsc::cpu_relax();
            }
            if (!t::is_valid(out, expected)) {
                if (bad_count++ == 0) first_bad_seq = expected;
            }
            consumed.store(expected + 1, std::memory_order_relaxed);
        }
    };

    const bool completed = t::run_producer_consumer(producer, consumer, abort, consumed);
    ASSERT_TRUE(completed) << "stalled after " << consumed.load() << "/" << n << " items";
    EXPECT_EQ(consumed.load(), n);
    EXPECT_EQ(bad_count, 0u) << "first bad event at expected seq " << first_bad_seq;
}

int env_cpu(const char* name, int fallback) {
    return static_cast<int>(t::env_u64(name, static_cast<std::uint64_t>(fallback)));
}

}  // namespace

template <typename Q>
class WraparoundStress : public ::testing::Test {};

// Order matters: scripts/mutation_test.sh selects instances 0-3 (all SpscQueue code paths).
using StressQueues = ::testing::Types<
    spsc::SpscQueue<OrderEvent, 2>,                                       // 0: default, tiny
    spsc::SpscQueue<OrderEvent, 4>,                                       // 1: default
    spsc::SpscQueue<OrderEvent, 8>,                                       // 2: default
    spsc::SpscQueue<OrderEvent, 4, spsc::kBaselineTuning>,                // 3: uncached path
    spsc::SpscQueue<OrderEvent, 4, spsc::Tuning{.prefetch_next = true}>,  // 4
    spsc::SpscQueue<OrderEvent, 4, spsc::Tuning{.seq_cst = true}>,        // 5
    spsc::MutexQueue<OrderEvent, 4>>;                                     // 6: baseline
TYPED_TEST_SUITE(WraparoundStress, StressQueues);

// Scheduler decides placement; preemption lands at arbitrary points.
TYPED_TEST(WraparoundStress, Unpinned) { run_stress<TypeParam>(-1, -1); }

// True parallelism across physical cores: coherence traffic through L3.
TYPED_TEST(WraparoundStress, PinnedDistinctCores) {
    run_stress<TypeParam>(env_cpu("SPSC_PRODUCER_CPU", 2), env_cpu("SPSC_CONSUMER_CPU", 4));
}

// SMT siblings share L1/L2: very tight interleaving at the full/empty boundary.
TYPED_TEST(WraparoundStress, PinnedSmtSiblings) {
    run_stress<TypeParam>(env_cpu("SPSC_PRODUCER_CPU", 2), env_cpu("SPSC_SMT_SIBLING_CPU", 3));
}
