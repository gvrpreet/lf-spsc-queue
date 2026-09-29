// Order pipeline example: intake thread -> SpscQueue -> matching thread.
//
// Intake (producer, pinned): generates a deterministic stream of order requests
// (new / cancel / modify, with ~1% deliberately invalid), validates them, and pushes
// valid ones as OrderEvents through the backpressure policy. Invalid requests are
// rejected at intake and never reach the matcher.
//
// Matcher (consumer, pinned): owns the order book exclusively, applies each event,
// and records end-to-end latency from request creation (tsc_intake) to the moment
// the acknowledgement is produced.
//
// At shutdown the program verifies the accounting (every enqueued event was
// applied exactly once) and the book's internal invariants.
//
// Usage:
//   order_pipeline [--requests=2000000] [--rate=1000000] [--producer-cpu=2]
//                  [--consumer-cpu=4] [--seed=42]
//   --rate is requests per second; 0 runs the intake as fast as possible.
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "order_book.hpp"
#include "platform.hpp"
#include "spsc/backpressure.hpp"
#include "spsc/cpu_relax.hpp"
#include "spsc/order_event.hpp"
#include "spsc/spsc_queue.hpp"

namespace {

constexpr std::int64_t kMidPrice = 10'000;
constexpr std::int64_t kMinPrice = kMidPrice - 500;
constexpr std::uint32_t kLevels = 1000;
constexpr std::uint32_t kMaxQty = 1'000;
constexpr std::uint32_t kInstrument = 7;

struct Request {
    spsc::EventType type;
    spsc::Side side;
    std::uint64_t order_id;
    std::int64_t price;
    std::uint32_t qty;
    std::uint32_t instrument;
};

// Intake-side validation; the matcher never sees a request that fails it.
bool validate(const Request& r) {
    if (r.instrument != kInstrument) return false;
    if (r.type == spsc::EventType::Cancel) return r.order_id != 0;
    return r.qty > 0 && r.qty <= kMaxQty && r.price >= kMinPrice &&
           r.price < kMinPrice + static_cast<std::int64_t>(kLevels);
}

// Deterministic request stream: ~70% new, ~20% cancel, ~10% modify, ~1% invalid.
// Buys cluster just below mid and sells just above, overlapping enough to trade.
class RequestGenerator {
public:
    explicit RequestGenerator(std::uint64_t seed) : rng_(seed) {}

    Request next() {
        Request r{spsc::EventType::New, spsc::Side::Buy, 0, 0, 0, kInstrument};
        const int kind = pct_(rng_);
        if (kind < 70 || next_id_ == 1) {
            r.type = spsc::EventType::New;
            r.order_id = next_id_++;
            r.side = (rng_() & 1) ? spsc::Side::Buy : spsc::Side::Sell;
            r.price = r.side == spsc::Side::Buy ? kMidPrice + offset_(rng_) - 8
                                                : kMidPrice - offset_(rng_) + 8;
            r.qty = qty_(rng_);
        } else if (kind < 90) {
            r.type = spsc::EventType::Cancel;
            r.order_id = pick_existing();
        } else {
            r.type = spsc::EventType::Modify;
            r.order_id = pick_existing();
            r.price = kMidPrice + offset_(rng_) - 10;
            r.qty = qty_(rng_);
        }
        if (pct_(rng_) == 0) corrupt(r);  // ~1%: exercise intake validation
        return r;
    }

    std::uint64_t ids_issued() const noexcept { return next_id_ - 1; }

private:
    std::uint64_t pick_existing() {
        // Mostly recent orders (still likely live), sometimes old ones (likely gone).
        const std::uint64_t back = std::min<std::uint64_t>(next_id_ - 1, recent_(rng_) + 1);
        return next_id_ - back;
    }
    void corrupt(Request& r) {
        switch (rng_() % 3) {
            case 0: r.qty = 0; r.type = spsc::EventType::New; break;
            case 1: r.price = kMidPrice + 100'000; r.type = spsc::EventType::New; break;
            default: r.instrument = kInstrument + 1; break;
        }
    }

    std::mt19937_64 rng_;
    std::uniform_int_distribution<int> pct_{0, 99};
    std::uniform_int_distribution<std::int64_t> offset_{0, 20};
    std::uniform_int_distribution<std::uint32_t> qty_{1, 100};
    std::uniform_int_distribution<std::uint64_t> recent_{0, 63};
    std::uint64_t next_id_ = 1;
};

struct MatcherCounters {
    std::uint64_t applied = 0;
    std::uint64_t acks = 0;
    std::uint64_t rejects = 0;  // cancel/modify of an order no longer live
};

}  // namespace

int main(int argc, char** argv) {
    const bench::Args args(argc, argv);
    const std::uint64_t requests = args.get_u64("requests", 2'000'000);
    const std::uint64_t rate = args.get_u64("rate", 1'000'000);
    const int producer_cpu = args.get_int("producer-cpu", 2);
    const int consumer_cpu = args.get_int("consumer-cpu", 4);
    const std::uint64_t seed = args.get_u64("seed", 42);

    const bench::Environment env = bench::probe_environment();
    bench::print_environment(env);
    const auto interval_cycles =
        rate == 0 ? 0 : static_cast<std::uint64_t>(env.tsc_ghz * 1e9 / static_cast<double>(rate));

    auto queue = std::make_unique<spsc::SpscQueue<spsc::OrderEvent, 65536>>();
    // Every new order gets a fresh id, so the pool needs at most `requests` slots.
    auto book = std::make_unique<pipeline::OrderBook>(
        kMinPrice, kLevels, static_cast<std::uint32_t>(requests + 1));

    std::vector<std::uint64_t> latency;  // cycles, intake -> ack
    latency.reserve(requests);
    std::atomic<bool> intake_done{false};
    spsc::PushStats push_stats;
    std::uint64_t intake_rejected = 0, enqueued = 0;
    MatcherCounters matcher;

    std::thread matching([&] {
        platform::pin_current_thread(consumer_cpu);
        spsc::OrderEvent ev{};
        auto apply = [&] {
            bool ok = false;
            const auto side = ev.side == spsc::Side::Buy ? pipeline::Side::Buy : pipeline::Side::Sell;
            switch (ev.type) {
                case spsc::EventType::New:
                    ok = book->add(ev.order_id, side, ev.price_ticks, ev.qty);
                    break;
                case spsc::EventType::Cancel:
                    ok = book->cancel(ev.order_id);
                    break;
                case spsc::EventType::Modify:
                    ok = book->modify(ev.order_id, ev.price_ticks, ev.qty);
                    break;
            }
            ++matcher.applied;
            ok ? ++matcher.acks : ++matcher.rejects;
            latency.push_back(platform::rdtsc_ordered() - ev.tsc_intake);  // reserved: no realloc
        };
        for (;;) {
            if (queue->try_pop(ev)) {
                apply();
            } else if (intake_done.load(std::memory_order_acquire)) {
                while (queue->try_pop(ev)) apply();
                break;
            } else {
                spsc::cpu_relax();
            }
        }
    });

    const auto t0 = std::chrono::steady_clock::now();
    std::thread intake([&] {
        platform::pin_current_thread(producer_cpu);
        RequestGenerator gen(seed);
        spsc::OrderEvent ev{};
        std::uint64_t next_send = platform::rdtsc();
        for (std::uint64_t i = 0; i < requests; ++i) {
            if (interval_cycles != 0) {
                while (platform::rdtsc() < next_send) spsc::cpu_relax();
                next_send += interval_cycles;
            }
            const std::uint64_t arrived = platform::rdtsc_ordered();
            const Request r = gen.next();
            if (!validate(r)) {
                ++intake_rejected;
                continue;
            }
            ev.seq = enqueued;
            ev.order_id = r.order_id;
            ev.price_ticks = r.price;
            ev.qty = r.qty;
            ev.instrument_id = r.instrument;
            ev.type = r.type;
            ev.side = r.side;
            ev.tsc_intake = arrived;
            ev.tsc_push = platform::rdtsc();
            if (spsc::push_with_backpressure(*queue, ev, push_stats)) {
                ++enqueued;
            }
        }
        intake_done.store(true, std::memory_order_release);
    });
    intake.join();
    matching.join();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    const bench::LatencySummary s = bench::summarize(latency, env.tsc_ghz);
    const bool accounting_ok = matcher.applied == enqueued &&
                               enqueued + intake_rejected + push_stats.rejected == requests;
    const bool book_ok = book->check_invariants();

    std::printf("requests          %" PRIu64 " in %.2f s (%.2f M/s offered%s)\n", requests, seconds,
                static_cast<double>(requests) / seconds / 1e6, rate == 0 ? ", unthrottled" : "");
    std::printf("intake rejected   %" PRIu64 " (failed validation)\n", intake_rejected);
    std::printf("enqueued          %" PRIu64 "  full_events=%" PRIu64 " backpressure_rejects=%" PRIu64 "\n",
                enqueued, push_stats.full_events, push_stats.rejected);
    std::printf("matcher applied   %" PRIu64 "  acks=%" PRIu64 " rejects=%" PRIu64 " (order not live)\n",
                matcher.applied, matcher.acks, matcher.rejects);
    std::printf("trades            %" PRIu64 "  traded_qty=%" PRIu64 "  resting_orders=%" PRIu64 "\n",
                book->stats().trades, book->stats().traded_qty, book->resting_orders());
    std::printf("latency intake->ack (ns): p50=%.0f p90=%.0f p99=%.0f p99.9=%.0f max=%.0f\n", s.p50,
                s.p90, s.p99, s.p999, s.max);
    std::printf("accounting        %s\n", accounting_ok ? "OK (every enqueued event applied once)" : "MISMATCH");
    std::printf("book invariants   %s\n", book_ok ? "OK (not crossed, levels consistent)" : "VIOLATED");
    return accounting_ok && book_ok ? 0 : 1;
}
