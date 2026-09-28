// Single-threaded contract tests. Typed over SpscQueue and the MutexQueue
// baseline: both must satisfy the same API contract.
#include <gtest/gtest.h>

#include <memory>

#include "spsc/mutex_queue.hpp"
#include "spsc/order_event.hpp"
#include "spsc/spsc_queue.hpp"
#include "test_util.hpp"

using spsc::OrderEvent;
using spsc::test::is_valid;
using spsc::test::make_event;

template <typename Q>
class QueueContract : public ::testing::Test {
protected:
    std::unique_ptr<Q> q = std::make_unique<Q>();
};

using Queues = ::testing::Types<spsc::SpscQueue<OrderEvent, 8>, spsc::MutexQueue<OrderEvent, 8>>;
TYPED_TEST_SUITE(QueueContract, Queues);

TYPED_TEST(QueueContract, StartsEmpty) {
    OrderEvent out{};
    EXPECT_FALSE(this->q->try_pop(out));
    EXPECT_EQ(this->q->size_approx(), 0u);
}

TYPED_TEST(QueueContract, PushThenPopRoundTrips) {
    ASSERT_TRUE(this->q->try_push(make_event(42)));
    OrderEvent out{};
    ASSERT_TRUE(this->q->try_pop(out));
    EXPECT_TRUE(is_valid(out, 42));
    EXPECT_FALSE(this->q->try_pop(out));
}

TYPED_TEST(QueueContract, PreservesFifoOrder) {
    for (std::uint64_t i = 0; i < 5; ++i) ASSERT_TRUE(this->q->try_push(make_event(i)));
    EXPECT_EQ(this->q->size_approx(), 5u);
    OrderEvent out{};
    for (std::uint64_t i = 0; i < 5; ++i) {
        ASSERT_TRUE(this->q->try_pop(out));
        EXPECT_TRUE(is_valid(out, i));
    }
}

TYPED_TEST(QueueContract, UsesFullCapacityThenRejects) {
    const auto cap = TypeParam::capacity();
    for (std::uint64_t i = 0; i < cap; ++i) ASSERT_TRUE(this->q->try_push(make_event(i))) << i;
    EXPECT_EQ(this->q->size_approx(), cap);
    EXPECT_FALSE(this->q->try_push(make_event(cap))) << "push into a full queue must fail";

    OrderEvent out{};
    ASSERT_TRUE(this->q->try_pop(out));
    EXPECT_TRUE(is_valid(out, 0));
    EXPECT_TRUE(this->q->try_push(make_event(cap))) << "one pop must free exactly one slot";
}

TYPED_TEST(QueueContract, WrapsAroundManyTimes) {
    // Keep 3 items in flight so indices cross the wrap boundary at every offset.
    std::uint64_t next_push = 0, next_pop = 0;
    OrderEvent out{};
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(this->q->try_push(make_event(next_push++)));
    for (int round = 0; round < 10'000; ++round) {
        ASSERT_TRUE(this->q->try_push(make_event(next_push++)));
        ASSERT_TRUE(this->q->try_pop(out));
        ASSERT_TRUE(is_valid(out, next_pop++)) << "round " << round;
    }
}

TEST(SpscQueueStatic, CapacityIsCompileTime) {
    static_assert(spsc::SpscQueue<OrderEvent, 65536>::capacity() == 65536);
    SUCCEED();
}
