#pragma once

// Single-instrument limit order book with price-time priority, owned by exactly one
// thread (the matcher), so it needs no synchronization.
//
// Everything is preallocated: orders live in a pool indexed by order id, and each
// price level is an intrusive doubly linked FIFO through that pool. Adding,
// cancelling, and matching allocate nothing.

#include <cstdint>
#include <vector>

namespace pipeline {

enum class Side : std::uint8_t { Buy, Sell };

struct BookStats {
    std::uint64_t trades = 0;
    std::uint64_t traded_qty = 0;
};

class OrderBook {
public:
    static constexpr std::uint32_t kNull = ~0u;

    // Prices are ticks in [min_price, min_price + levels); order ids in [1, max_orders].
    OrderBook(std::int64_t min_price, std::uint32_t levels, std::uint32_t max_orders)
        : min_price_(min_price), levels_(levels), bids_(levels), asks_(levels),
          orders_(static_cast<std::size_t>(max_orders) + 1) {}

    bool valid_price(std::int64_t price) const noexcept {
        return price >= min_price_ && price < min_price_ + static_cast<std::int64_t>(levels_);
    }
    bool valid_id(std::uint64_t id) const noexcept { return id >= 1 && id < orders_.size(); }
    bool is_live(std::uint64_t id) const noexcept { return valid_id(id) && orders_[id].live; }

    // New limit order: matches against the opposite side, rests any remainder.
    // Returns false if the id is out of range or already live.
    bool add(std::uint64_t id, Side side, std::int64_t price, std::uint32_t qty) {
        if (!valid_id(id) || orders_[id].live || !valid_price(price) || qty == 0) return false;
        const auto level = static_cast<std::uint32_t>(price - min_price_);
        qty = match(side, level, qty);
        if (qty > 0) rest(static_cast<std::uint32_t>(id), side, level, qty);
        return true;
    }

    // Returns false if the order is not live (already filled or cancelled).
    bool cancel(std::uint64_t id) {
        if (!is_live(id)) return false;
        unlink(static_cast<std::uint32_t>(id));
        return true;
    }

    // Cancel/replace: the order loses time priority and may match at its new price.
    bool modify(std::uint64_t id, std::int64_t new_price, std::uint32_t new_qty) {
        if (!is_live(id) || !valid_price(new_price) || new_qty == 0) return false;
        const Side side = orders_[id].side;
        unlink(static_cast<std::uint32_t>(id));
        return add(id, side, new_price, new_qty);
    }

    // Best prices, or kNull if that side is empty.
    std::uint32_t best_bid_level() const noexcept { return best_bid_; }
    std::uint32_t best_ask_level() const noexcept { return best_ask_; }
    bool crossed() const noexcept {
        return best_bid_ != kNull && best_ask_ != kNull && best_bid_ >= best_ask_;
    }

    std::uint64_t resting_orders() const noexcept { return resting_; }
    const BookStats& stats() const noexcept { return stats_; }

    // Walks every level and checks that the cached quantities, counts and best
    // prices agree with the linked lists. O(levels + orders); for tests/shutdown only.
    bool check_invariants() const {
        std::uint64_t counted = 0;
        std::uint32_t best_bid = kNull, best_ask = kNull;
        for (std::uint32_t lv = 0; lv < levels_; ++lv) {
            for (int s = 0; s < 2; ++s) {
                const Level& level = s == 0 ? bids_[lv] : asks_[lv];
                std::uint64_t qty = 0;
                for (std::uint32_t i = level.head; i != kNull; i = orders_[i].next) {
                    if (!orders_[i].live || orders_[i].level != lv) return false;
                    qty += orders_[i].qty;
                    ++counted;
                }
                if (qty != level.qty) return false;
                if (qty > 0 && s == 0) best_bid = lv;
                if (qty > 0 && s == 1 && best_ask == kNull) best_ask = lv;
            }
        }
        return counted == resting_ && best_bid == best_bid_ && best_ask == best_ask_ && !crossed();
    }

private:
    struct Order {
        std::uint32_t qty = 0;
        std::uint32_t level = 0;
        std::uint32_t prev = kNull;
        std::uint32_t next = kNull;
        Side side = Side::Buy;
        bool live = false;
    };
    struct Level {
        std::uint32_t head = kNull;
        std::uint32_t tail = kNull;
        std::uint64_t qty = 0;
    };

    std::uint32_t match(Side side, std::uint32_t limit, std::uint32_t qty) {
        const bool buy = side == Side::Buy;
        while (qty > 0) {
            const std::uint32_t best = buy ? best_ask_ : best_bid_;
            if (best == kNull || (buy ? best > limit : best < limit)) break;
            Level& level = buy ? asks_[best] : bids_[best];
            const std::uint32_t maker = level.head;
            Order& o = orders_[maker];
            const std::uint32_t fill = o.qty < qty ? o.qty : qty;
            qty -= fill;
            o.qty -= fill;
            level.qty -= fill;
            ++stats_.trades;
            stats_.traded_qty += fill;
            if (o.qty == 0) unlink(maker);
        }
        return qty;
    }

    void rest(std::uint32_t id, Side side, std::uint32_t lv, std::uint32_t qty) {
        Order& o = orders_[id];
        o = Order{qty, lv, kNull, kNull, side, true};
        Level& level = side == Side::Buy ? bids_[lv] : asks_[lv];
        o.prev = level.tail;
        if (level.tail != kNull) {
            orders_[level.tail].next = id;
        } else {
            level.head = id;
        }
        level.tail = id;
        level.qty += qty;
        ++resting_;
        if (side == Side::Buy) {
            if (best_bid_ == kNull || lv > best_bid_) best_bid_ = lv;
        } else {
            if (best_ask_ == kNull || lv < best_ask_) best_ask_ = lv;
        }
    }

    void unlink(std::uint32_t id) {
        Order& o = orders_[id];
        Level& level = o.side == Side::Buy ? bids_[o.level] : asks_[o.level];
        (o.prev != kNull ? orders_[o.prev].next : level.head) = o.next;
        (o.next != kNull ? orders_[o.next].prev : level.tail) = o.prev;
        level.qty -= o.qty;
        o.live = false;
        --resting_;
        if (level.head == kNull) refresh_best(o.side, o.level);
    }

    // Called when `lv` became empty; moves the best price outward if it was the best.
    void refresh_best(Side side, std::uint32_t lv) {
        if (side == Side::Buy && lv == best_bid_) {
            while (best_bid_ != kNull && bids_[best_bid_].head == kNull) {
                best_bid_ = best_bid_ == 0 ? kNull : best_bid_ - 1;
            }
        } else if (side == Side::Sell && lv == best_ask_) {
            while (best_ask_ != kNull && asks_[best_ask_].head == kNull) {
                best_ask_ = best_ask_ + 1 == levels_ ? kNull : best_ask_ + 1;
            }
        }
    }

    std::int64_t min_price_;
    std::uint32_t levels_;
    std::vector<Level> bids_;
    std::vector<Level> asks_;
    std::vector<Order> orders_;
    std::uint32_t best_bid_ = kNull;
    std::uint32_t best_ask_ = kNull;
    std::uint64_t resting_ = 0;
    BookStats stats_;
};

}  // namespace pipeline
