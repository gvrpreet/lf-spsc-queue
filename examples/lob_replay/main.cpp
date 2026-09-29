// Two-thread market replay on lob-engine (github.com/gvrpreet/lob-engine):
//
//   reader thread (feed handler)  --SpscQueue<Event>-->  engine thread (lob::OrderBook)
//
// The engine thread applies every event exactly as lob-engine's single-threaded replay loop
// does (same book type, same accounting). The program then checks that the final book
// checksum and every counter are identical to replay::replay() on the same log. It reports
// events/s for the single-threaded replay and for the two-thread pipeline over the SPSC
// queue (32-byte or cache-line-padded slots) and over the mutex baseline. With --queue=all or
// --stream=1 it also compares a streaming reader (buffered reads plus validation) against
// lob-engine's single-threaded replay_stream().
//
// Usage:
//   lob_replay <events.bin> [--queue=all|spsc|spsc-padded|mutex] [--reps=3]
//              [--producer-cpu=2] [--consumer-cpu=4] [--csv=out.csv] [--label=name]
// Exit status is non-zero if any two-thread run diverges from the single-threaded replay.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "lob/order_book.hpp"
#include "platform.hpp"
#include "replay/event.hpp"
#include "replay/event_stream.hpp"
#include "replay/mapped_event_log.hpp"
#include "replay/replayer.hpp"
#include "spsc/cpu_relax.hpp"
#include "spsc/mutex_queue.hpp"
#include "spsc/spsc_queue.hpp"

namespace {

constexpr std::size_t kCapacity = 65536;
constexpr std::size_t kBookCapacity = 1u << 20;  // replay::ReplayConfig's default

// One event per cache line: the producer writing slot i+1 never shares a line with the
// consumer reading slot i. Costs twice the ring memory.
struct alignas(64) PaddedEvent {
    replay::Event event;
};
static_assert(sizeof(PaddedEvent) == 64);

// Order-sensitive hash of an event sequence. The engine thread folds in every event it pops;
// equality with the hash of the source log proves the queue delivered exactly the same events
// in the same order, independently of what the book does with them.
struct SequenceHash {
    std::uint64_t h = 0x243F6A8885A308D3ull;
    void add(const replay::Event& e) noexcept {
        std::uint64_t w[4];
        static_assert(sizeof(w) == sizeof(e));
        std::memcpy(w, &e, sizeof(e));
        for (std::uint64_t x : w) {
            h = (h ^ x) * 0x9E3779B97F4A7C15ull;
            h ^= h >> 29;
        }
    }
};

struct Result {
    std::uint64_t events = 0, trades = 0, volume = 0, rejected = 0;
    std::uint64_t cancel_misses = 0, modify_misses = 0, reduce_misses = 0, final_orders = 0;
    std::uint64_t checksum = 0;
    std::uint64_t sequence_hash = 0;  // 0 when --verify is off
    double seconds = 0;

    bool same_book_as(const replay::ReplayStats& r) const {
        return events == r.events && trades == r.trades && volume == r.volume &&
               rejected == r.rejected && cancel_misses == r.cancel_misses &&
               modify_misses == r.modify_misses && reduce_misses == r.reduce_misses &&
               final_orders == r.final_orders && checksum == r.checksum;
    }
};

const replay::Event& unwrap(const replay::Event& e) { return e; }
const replay::Event& unwrap(const PaddedEvent& e) { return e.event; }
replay::Event wrap(const replay::Event& e, replay::Event*) { return e; }
PaddedEvent wrap(const replay::Event& e, PaddedEvent*) { return PaddedEvent{e}; }

// Mirrors replay_with_book() in lob-engine's replay/src/replayer.cpp.
void apply(lob::OrderBook& book, const replay::Event& ev, Result& r) {
    const auto sink = [&r](const lob::Trade& trade) {
        ++r.trades;
        r.volume += trade.qty;
    };
    switch (ev.type) {
        case replay::EventType::New:
            if (book.add(ev.id, ev.side, ev.otype, ev.price, ev.qty, ev.ts, sink) ==
                lob::AddResult::Rejected) {
                ++r.rejected;
            }
            break;
        case replay::EventType::Cancel:
            if (!book.cancel(ev.id)) ++r.cancel_misses;
            break;
        case replay::EventType::Modify: {
            const auto m = book.modify(ev.id, ev.price, ev.qty, ev.ts, sink);
            if (m == lob::ModifyResult::NotFound) {
                ++r.modify_misses;
            } else if (m == lob::ModifyResult::Rejected) {
                ++r.rejected;
            }
            break;
        }
        case replay::EventType::Reduce:
        case replay::EventType::Execute:
            if (!book.reduce(ev.id, ev.qty)) ++r.reduce_misses;
            break;
    }
    ++r.events;
}

template <typename Slot, typename Queue>
Result run_two_thread(std::span<const replay::Event> events, int producer_cpu, int consumer_cpu,
                      bool verify) {
    using clock = std::chrono::steady_clock;
    auto q = std::make_unique<Queue>();
    std::atomic<bool> go{false};
    Result result;
    clock::time_point t_end;

    std::thread engine([&] {
        platform::pin_current_thread(consumer_cpu);
        lob::OrderBook book(kBookCapacity);  // constructed on the thread that owns it
        Slot slot{};
        SequenceHash seq;
        while (!go.load(std::memory_order_acquire)) spsc::cpu_relax();
        for (std::size_t i = 0; i < events.size(); ++i) {
            while (!q->try_pop(slot)) spsc::cpu_relax();
            if (verify) seq.add(unwrap(slot));
            apply(book, unwrap(slot), result);
        }
        t_end = clock::now();
        result.final_orders = book.order_count();
        result.checksum = book.checksum();
        result.sequence_hash = verify ? seq.h : 0;
    });
    std::thread reader([&] {
        platform::pin_current_thread(producer_cpu);
        while (!go.load(std::memory_order_acquire)) spsc::cpu_relax();
        for (const replay::Event& e : events) {
            const Slot slot = wrap(e, static_cast<Slot*>(nullptr));
            while (!q->try_push(slot)) spsc::cpu_relax();  // lossless: a replay may not drop
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto t_start = clock::now();
    go.store(true, std::memory_order_release);
    reader.join();
    engine.join();
    result.seconds = std::chrono::duration<double>(t_end - t_start).count();
    return result;
}

// Streaming source: the reader thread does real work (buffered file reads plus lob-engine's
// per-chunk validation), which the pipeline overlaps with matching on the engine thread.
template <typename Queue>
Result run_two_thread_stream(const std::string& path, std::uint64_t count, int producer_cpu,
                             int consumer_cpu, bool verify) {
    using clock = std::chrono::steady_clock;
    auto q = std::make_unique<Queue>();
    std::atomic<bool> go{false};
    Result result;
    clock::time_point t_end;

    std::thread engine([&] {
        platform::pin_current_thread(consumer_cpu);
        lob::OrderBook book(kBookCapacity);
        replay::Event ev{};
        SequenceHash seq;
        while (!go.load(std::memory_order_acquire)) spsc::cpu_relax();
        for (std::uint64_t i = 0; i < count; ++i) {
            while (!q->try_pop(ev)) spsc::cpu_relax();
            if (verify) seq.add(ev);
            apply(book, ev, result);
        }
        t_end = clock::now();
        result.final_orders = book.order_count();
        result.checksum = book.checksum();
        result.sequence_hash = verify ? seq.h : 0;
    });
    std::thread reader([&] {
        platform::pin_current_thread(producer_cpu);
        replay::EventLogStream stream;
        std::string error;
        if (!stream.open(path, &error)) {
            std::fprintf(stderr, "stream open failed: %s\n", error.c_str());
            std::exit(2);
        }
        while (!go.load(std::memory_order_acquire)) spsc::cpu_relax();
        std::span<const replay::Event> chunk;
        while (stream.next(chunk, &error) && !chunk.empty()) {
            for (const replay::Event& e : chunk) {
                while (!q->try_push(e)) spsc::cpu_relax();
            }
        }
        if (!error.empty()) {
            std::fprintf(stderr, "stream read failed: %s\n", error.c_str());
            std::exit(2);
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto t_start = clock::now();
    go.store(true, std::memory_order_release);
    reader.join();
    engine.join();
    result.seconds = std::chrono::duration<double>(t_end - t_start).count();
    return result;
}

struct Variant {
    const char* name;
    Result (*run)(std::span<const replay::Event>, int, int, bool);
};

constexpr Variant kVariants[] = {
    {"spsc", run_two_thread<replay::Event, spsc::SpscQueue<replay::Event, kCapacity>>},
    {"spsc-padded", run_two_thread<PaddedEvent, spsc::SpscQueue<PaddedEvent, kCapacity>>},
    {"mutex", run_two_thread<replay::Event, spsc::MutexQueue<replay::Event, kCapacity>>},
};

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argv[1][0] == '-') {
        std::fprintf(stderr, "usage: %s <events.bin> [--queue=all|spsc|spsc-padded|mutex] [--reps=3] "
                             "[--verify=0|1] [--producer-cpu=2] [--consumer-cpu=4] [--csv=out.csv] "
                             "[--label=name]\n",
                     argv[0]);
        return 2;
    }
    const std::string path = argv[1];
    const bench::Args args(argc - 1, argv + 1);
    const std::string which = args.get("queue", "all");
    const int reps = std::max(1, args.get_int("reps", 3));
    // --verify=1 hashes every event the engine thread receives (costs a few ns per event, so
    // it is off for throughput measurements; the book checksum is always compared).
    const bool verify = args.get_u64("verify", 0) != 0;
    const int producer_cpu = args.get_int("producer-cpu", 2);
    const int consumer_cpu = args.get_int("consumer-cpu", 4);
    std::string label = args.get("label", "");
    if (label.empty()) label = path.substr(path.find_last_of('/') + 1);

    replay::MappedEventLog log;
    std::string error;
    if (!log.open(path, &error)) {
        std::fprintf(stderr, "cannot open %s: %s\n", path.c_str(), error.c_str());
        return 2;
    }
    const std::span<const replay::Event> events = log.events();
    SequenceHash source;
    if (verify) {
        for (const replay::Event& e : events) source.add(e);
    }
    const auto delivered_intact = [&](const Result& r) { return !verify || r.sequence_hash == source.h; };

    // Reference: lob-engine's own single-threaded replay, pinned to the engine's CPU.
    std::vector<double> ref_rates;
    replay::ReplayStats ref;
    {
        const platform::AffinityGuard restore;  // threads created later must not inherit the pin
        platform::pin_current_thread(consumer_cpu);
        for (int i = 0; i < reps; ++i) {
            ref = replay::replay(events);
            ref_rates.push_back(ref.events_per_sec());
        }
    }
    const double ref_rate = median(ref_rates);
    std::printf("%-12s %-12s events=%" PRIu64 " trades=%" PRIu64 " checksum=%016" PRIx64
                " | %.2f M events/s (median of %d)\n",
                label.c_str(), "single", ref.events, ref.trades, ref.checksum, ref_rate / 1e6, reps);
    bench::append_csv_row(args.get("csv", ""),
                          "label,mode,events,trades,checksum,matches_single_thread,median_events_per_sec",
                          label + ",single," + std::to_string(ref.events) + "," +
                              std::to_string(ref.trades) + "," + std::to_string(ref.checksum) +
                              ",1," + std::to_string(static_cast<std::uint64_t>(ref_rate)));

    bool all_match = true;
    for (const Variant& v : kVariants) {
        if (which != "all" && which != v.name) continue;
        std::vector<double> rates;
        bool match = true;
        Result r;
        for (int i = 0; i < reps; ++i) {
            r = v.run(events, producer_cpu, consumer_cpu, verify);
            match = match && r.same_book_as(ref) && delivered_intact(r);
            rates.push_back(static_cast<double>(r.events) / r.seconds);
        }
        all_match = all_match && match;
        const double rate = median(rates);
        std::printf("%-12s %-12s events=%" PRIu64 " trades=%" PRIu64 " checksum=%016" PRIx64
                    " | %.2f M events/s (%+.0f%% vs single) | %s\n",
                    label.c_str(), v.name, r.events, r.trades, r.checksum, rate / 1e6,
                    (rate / ref_rate - 1.0) * 100.0,
                    match ? "identical to single-threaded replay" : "MISMATCH");
        bench::append_csv_row(args.get("csv", ""),
                              "label,mode,events,trades,checksum,matches_single_thread,median_events_per_sec",
                              label + "," + v.name + "," + std::to_string(r.events) + "," +
                                  std::to_string(r.trades) + "," + std::to_string(r.checksum) + "," +
                                  (match ? "1" : "0") + "," +
                                  std::to_string(static_cast<std::uint64_t>(rate)));
    }

    if (!args.get("stream", "").empty() || which == "all") {
        // Streaming: single-threaded replay_stream (read + validate + match on one thread)
        // versus the two-thread pipeline with the same reader on the producer side.
        std::vector<double> single_rates;
        for (int i = 0; i < reps; ++i) {
            const platform::AffinityGuard restore;
            platform::pin_current_thread(consumer_cpu);
            replay::EventLogStream stream;
            replay::ReplayStats s;
            if (!stream.open(path, &error) || !replay::replay_stream(stream, {}, s, nullptr, &error)) {
                std::fprintf(stderr, "replay_stream failed: %s\n", error.c_str());
                return 2;
            }
            all_match = all_match && s.checksum == ref.checksum && s.events == ref.events;
            single_rates.push_back(s.events_per_sec());
        }
        const double stream_ref = median(single_rates);
        std::printf("%-12s %-12s events=%" PRIu64 " | %.2f M events/s (median of %d)\n",
                    label.c_str(), "stream-single", ref.events, stream_ref / 1e6, reps);
        bench::append_csv_row(args.get("csv", ""),
                              "label,mode,events,trades,checksum,matches_single_thread,median_events_per_sec",
                              label + ",stream-single," + std::to_string(ref.events) + "," +
                                  std::to_string(ref.trades) + "," + std::to_string(ref.checksum) +
                                  ",1," + std::to_string(static_cast<std::uint64_t>(stream_ref)));

        const std::pair<const char*, Result (*)(const std::string&, std::uint64_t, int, int, bool)>
            stream_variants[] = {
                {"stream-spsc", run_two_thread_stream<spsc::SpscQueue<replay::Event, kCapacity>>},
                {"stream-mutex", run_two_thread_stream<spsc::MutexQueue<replay::Event, kCapacity>>},
            };
        for (const auto& [name, run] : stream_variants) {
            std::vector<double> rates;
            bool match = true;
            Result r;
            for (int i = 0; i < reps; ++i) {
                r = run(path, ref.events, producer_cpu, consumer_cpu, verify);
                match = match && r.same_book_as(ref) && delivered_intact(r);
                rates.push_back(static_cast<double>(r.events) / r.seconds);
            }
            all_match = all_match && match;
            const double rate = median(rates);
            std::printf("%-12s %-12s events=%" PRIu64 " trades=%" PRIu64 " checksum=%016" PRIx64
                        " | %.2f M events/s (%+.0f%% vs stream-single) | %s\n",
                        label.c_str(), name, r.events, r.trades, r.checksum, rate / 1e6,
                        (rate / stream_ref - 1.0) * 100.0,
                        match ? "identical to single-threaded replay" : "MISMATCH");
            bench::append_csv_row(args.get("csv", ""),
                                  "label,mode,events,trades,checksum,matches_single_thread,median_events_per_sec",
                                  label + "," + name + "," + std::to_string(r.events) + "," +
                                      std::to_string(r.trades) + "," + std::to_string(r.checksum) +
                                      "," + (match ? "1" : "0") + "," +
                                      std::to_string(static_cast<std::uint64_t>(rate)));
        }
    }
    return all_match ? 0 : 1;
}
