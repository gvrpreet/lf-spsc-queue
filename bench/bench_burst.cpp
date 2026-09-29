// Bursty-load benchmark.
//
// The producer follows a fixed on/off schedule: bursts of --burst-size events spaced
// --spacing-ns apart (faster than the consumer can drain), separated by idle gaps of
// --gap-us. Each event's latency is measured from its *intended* send time in the
// schedule, not from when the producer actually managed to push it. If the producer
// is held up by a full queue or a held lock, every event scheduled during that stall
// is charged for it, which avoids coordinated omission.
//
// The producer pushes through spsc::push_with_backpressure, so full-queue events and
// explicit rejections are reported. Queue depth is sampled every 64 pushes.
//
// Usage:
//   bench_burst [--queue=spsc] [--bursts=1000] [--burst-size=4096] [--spacing-ns=20]
//               [--gap-us=1000] [--service-ns=0] [--reject-after-us=1000]
//               [--producer-cpu=2] [--consumer-cpu=4] [--label=name] [--csv=out.csv]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "queue_variants.hpp"
#include "spsc/backpressure.hpp"
#include "spsc/cpu_relax.hpp"
#include "spsc/order_event.hpp"

namespace {

struct Config {
    std::string queue;
    std::string label;
    std::uint64_t bursts;
    std::uint64_t burst_size;
    std::uint64_t spacing_ns;
    std::uint64_t gap_us;
    std::uint64_t service_ns;
    std::uint64_t reject_after_us;
    int producer_cpu;
    int consumer_cpu;
};

struct RunOutput {
    std::vector<std::uint64_t> samples;  // cycles from intended send time to pop
    spsc::PushStats stats;
    std::uint64_t max_depth = 0;
    double seconds = 0.0;
};

template <typename Queue>
RunOutput run(const Config& cfg, const bench::Environment& env) {
    using clock = std::chrono::steady_clock;
    auto q = std::make_unique<Queue>();
    const std::uint64_t total = cfg.bursts * cfg.burst_size;
    const double ghz = env.tsc_ghz;
    const auto spacing = static_cast<std::uint64_t>(static_cast<double>(cfg.spacing_ns) * ghz);
    const auto gap = static_cast<std::uint64_t>(static_cast<double>(cfg.gap_us) * 1000.0 * ghz);
    const auto service = static_cast<std::uint64_t>(static_cast<double>(cfg.service_ns) * ghz);
    const spsc::BackpressureConfig bp{
        .reject_after = std::chrono::microseconds(cfg.reject_after_us)};

    RunOutput out;
    out.samples.reserve(total);
    std::atomic<bool> producer_done{false};
    std::uint64_t max_depth = 0;
    std::uint64_t start_tsc = 0;
    std::atomic<bool> go{false};

    std::thread consumer([&] {
        platform::pin_current_thread(cfg.consumer_cpu);
        spsc::OrderEvent ev{};
        auto handle = [&] {
            out.samples.push_back(platform::rdtsc_ordered() - ev.tsc_intake);  // reserved: no realloc
            if (service != 0) {
                const std::uint64_t until = platform::rdtsc() + service;
                while (platform::rdtsc() < until) {
                }
            }
        };
        for (;;) {
            if (q->try_pop(ev)) {
                handle();
            } else if (producer_done.load(std::memory_order_acquire)) {
                while (q->try_pop(ev)) handle();
                break;
            } else {
                spsc::cpu_relax();
            }
        }
    });
    std::thread producer([&] {
        platform::pin_current_thread(cfg.producer_cpu);
        while (!go.load(std::memory_order_acquire)) spsc::cpu_relax();
        spsc::OrderEvent ev{};
        std::uint64_t seq = 0;
        std::uint64_t burst_start = start_tsc;
        for (std::uint64_t b = 0; b < cfg.bursts; ++b) {
            for (std::uint64_t k = 0; k < cfg.burst_size; ++k, ++seq) {
                const std::uint64_t intended = burst_start + k * spacing;
                while (platform::rdtsc() < intended) spsc::cpu_relax();
                ev.seq = seq;
                ev.tsc_intake = intended;
                spsc::push_with_backpressure(*q, ev, out.stats, bp);
                if ((seq & 63) == 0) max_depth = std::max<std::uint64_t>(max_depth, q->size_approx());
            }
            burst_start += cfg.burst_size * spacing + gap;
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    start_tsc = platform::rdtsc() + static_cast<std::uint64_t>(1e6 * ghz);  // start in 1 ms
    const auto t0 = clock::now();
    go.store(true, std::memory_order_release);
    producer.join();
    consumer.join();
    out.seconds = std::chrono::duration<double>(clock::now() - t0).count();
    out.max_depth = max_depth;

    if (out.samples.size() != out.stats.pushed || out.stats.pushed + out.stats.rejected != total) {
        std::fprintf(stderr, "accounting mismatch: popped=%zu pushed=%" PRIu64 " rejected=%" PRIu64
                             " total=%" PRIu64 "\n",
                     out.samples.size(), out.stats.pushed, out.stats.rejected, total);
        std::exit(1);
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    const bench::Args args(argc, argv);
    Config cfg{
        args.get("queue", "spsc"),
        args.get("label", ""),
        args.get_u64("bursts", 1000),
        args.get_u64("burst-size", 4096),
        args.get_u64("spacing-ns", 20),
        args.get_u64("gap-us", 1000),
        args.get_u64("service-ns", 0),
        args.get_u64("reject-after-us", 1000),
        args.get_int("producer-cpu", 2),
        args.get_int("consumer-cpu", 4),
    };
    if (cfg.label.empty()) cfg.label = cfg.queue;

    const bench::Environment env = bench::probe_environment();
    bench::print_environment(env);

    RunOutput out;
    if (!bench::with_queue(cfg.queue, [&]<typename Queue>() { out = run<Queue>(cfg, env); })) {
        return 2;
    }

    const bench::LatencySummary s = bench::summarize(out.samples, env.tsc_ghz);
    std::printf("%-12s bursts=%" PRIu64 "x%" PRIu64 " spacing=%" PRIu64 "ns gap=%" PRIu64
                "us service=%" PRIu64 "ns | p50=%.0f p90=%.0f p99=%.0f p99.9=%.0f max=%.0f (ns) | "
                "max_depth=%" PRIu64 " full_events=%" PRIu64 " rejected=%" PRIu64 "\n",
                cfg.label.c_str(), cfg.bursts, cfg.burst_size, cfg.spacing_ns, cfg.gap_us,
                cfg.service_ns, s.p50, s.p90, s.p99, s.p999, s.max, out.max_depth,
                out.stats.full_events, out.stats.rejected);

    char row[512];
    std::snprintf(row, sizeof(row),
                  "%s,%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                  ",%.1f,%.1f,%.1f,%.1f,%.1f,%" PRIu64 ",%" PRIu64 ",%" PRIu64,
                  cfg.label.c_str(), cfg.queue.c_str(), cfg.bursts, cfg.burst_size, cfg.spacing_ns,
                  cfg.gap_us, cfg.service_ns, s.p50, s.p90, s.p99, s.p999, s.max, out.max_depth,
                  out.stats.full_events, out.stats.rejected);
    bench::append_csv_row(args.get("csv", ""),
                          "label,queue,bursts,burst_size,spacing_ns,gap_us,service_ns,"
                          "p50_ns,p90_ns,p99_ns,p999_ns,max_ns,max_depth,full_events,rejected",
                          row);
    return 0;
}
