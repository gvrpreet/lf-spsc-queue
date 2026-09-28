#pragma once

#include <cstdint>
#include <type_traits>

namespace spsc {

enum class EventType : std::uint8_t { New, Cancel, Modify };
enum class Side : std::uint8_t { Buy, Sell };

// One queue slot == one cache line. Stored by value in the ring: no heap
// allocation or pointer-chasing on the hot path.
struct alignas(64) OrderEvent {
    std::uint64_t seq;           // intake sequence number, monotonic per producer
    std::uint64_t order_id;
    std::uint64_t client_id;
    std::int64_t  price_ticks;   // fixed-point price; never floating point
    std::uint32_t qty;
    std::uint32_t instrument_id;
    std::uint64_t tsc_intake;    // rdtsc when the request arrived (end-to-end latency)
    std::uint64_t tsc_push;      // rdtsc just before try_push (queue latency)
    EventType     type;
    Side          side;
    std::uint8_t  reserved[2];
    std::uint32_t checksum;      // used by tests to detect torn/early reads
};

static_assert(sizeof(OrderEvent) == 64, "OrderEvent must be exactly one cache line");
static_assert(alignof(OrderEvent) == 64);
static_assert(std::is_trivially_copyable_v<OrderEvent>);

}  // namespace spsc
