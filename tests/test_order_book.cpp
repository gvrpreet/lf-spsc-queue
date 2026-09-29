// Order book used by the pipeline example: price-time priority and invariants.
#include <gtest/gtest.h>

#include <random>

#include "order_book.hpp"

using pipeline::OrderBook;
using pipeline::Side;

namespace {
constexpr std::int64_t kMin = 100;
OrderBook make_book() { return OrderBook(kMin, 100, 1000); }  // prices 100..199
std::uint32_t level(std::int64_t price) { return static_cast<std::uint32_t>(price - kMin); }
}  // namespace

TEST(OrderBook, RestsNonCrossingOrders) {
    auto b = make_book();
    ASSERT_TRUE(b.add(1, Side::Buy, 150, 10));
    ASSERT_TRUE(b.add(2, Side::Sell, 152, 5));
    EXPECT_EQ(b.best_bid_level(), level(150));
    EXPECT_EQ(b.best_ask_level(), level(152));
    EXPECT_EQ(b.stats().trades, 0u);
    EXPECT_EQ(b.resting_orders(), 2u);
    EXPECT_TRUE(b.check_invariants());
}

TEST(OrderBook, MatchesAtRestingPriceInTimePriority) {
    auto b = make_book();
    b.add(1, Side::Sell, 151, 5);
    b.add(2, Side::Sell, 151, 5);  // same level, later in time
    b.add(3, Side::Sell, 152, 5);
    ASSERT_TRUE(b.add(4, Side::Buy, 152, 7));  // takes all of #1, 2 of #2
    EXPECT_EQ(b.stats().trades, 2u);
    EXPECT_EQ(b.stats().traded_qty, 7u);
    EXPECT_FALSE(b.is_live(1));
    EXPECT_TRUE(b.is_live(2));
    EXPECT_FALSE(b.is_live(4)) << "fully filled aggressor must not rest";
    EXPECT_EQ(b.best_ask_level(), level(151));
    EXPECT_TRUE(b.check_invariants());
}

TEST(OrderBook, SweepsLevelsAndRestsRemainder) {
    auto b = make_book();
    b.add(1, Side::Sell, 151, 5);
    b.add(2, Side::Sell, 153, 5);
    ASSERT_TRUE(b.add(3, Side::Buy, 152, 20));  // fills #1 only; 15 rests at 152
    EXPECT_EQ(b.stats().traded_qty, 5u);
    EXPECT_TRUE(b.is_live(3));
    EXPECT_EQ(b.best_bid_level(), level(152));
    EXPECT_EQ(b.best_ask_level(), level(153));
    EXPECT_TRUE(b.check_invariants());
}

TEST(OrderBook, CancelAndModify) {
    auto b = make_book();
    b.add(1, Side::Buy, 150, 10);
    b.add(2, Side::Buy, 149, 10);
    EXPECT_TRUE(b.cancel(1));
    EXPECT_FALSE(b.cancel(1)) << "double cancel must be rejected";
    EXPECT_EQ(b.best_bid_level(), level(149));
    EXPECT_TRUE(b.modify(2, 155, 3));
    EXPECT_EQ(b.best_bid_level(), level(155));
    EXPECT_FALSE(b.modify(99, 150, 1)) << "unknown order";
    EXPECT_TRUE(b.check_invariants());
}

TEST(OrderBook, RejectsInvalidInput) {
    auto b = make_book();
    EXPECT_FALSE(b.add(0, Side::Buy, 150, 1));      // id 0 reserved
    EXPECT_FALSE(b.add(1, Side::Buy, 50, 1));       // below band
    EXPECT_FALSE(b.add(1, Side::Buy, 200, 1));      // above band
    EXPECT_FALSE(b.add(1, Side::Buy, 150, 0));      // zero qty
    ASSERT_TRUE(b.add(1, Side::Buy, 150, 1));
    EXPECT_FALSE(b.add(1, Side::Buy, 150, 1));      // duplicate live id
}

TEST(OrderBook, RandomizedInvariants) {
    OrderBook b(kMin, 100, 20'000);
    std::mt19937 rng(7);
    std::uniform_int_distribution<std::uint32_t> qty(1, 50);
    std::uint64_t next_id = 1;
    for (int i = 0; i < 15'000; ++i) {
        const int kind = static_cast<int>(rng() % 10);
        if (kind < 6 || next_id == 1) {
            const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
            const std::int64_t price = side == Side::Buy ? 145 + static_cast<std::int64_t>(rng() % 10)
                                                         : 150 + static_cast<std::int64_t>(rng() % 10) - 4;
            b.add(next_id++, side, price, qty(rng));
        } else if (kind < 9) {
            b.cancel(1 + rng() % (next_id - 1));
        } else {
            b.modify(1 + rng() % (next_id - 1), 140 + static_cast<std::int64_t>(rng() % 20), qty(rng));
        }
        ASSERT_FALSE(b.crossed()) << "step " << i;
        if (i % 1000 == 0) {
            ASSERT_TRUE(b.check_invariants()) << "step " << i;
        }
    }
    EXPECT_TRUE(b.check_invariants());
    EXPECT_GT(b.stats().trades, 0u);
}
