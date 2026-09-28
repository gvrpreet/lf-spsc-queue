// Push -> pop one-way latency under paced load.
//
// The producer stamps ev.tsc_push once, before the first push attempt, so any time
// spent retrying against a full queue or a held mutex is included in the sample.
// The consumer records the TSC immediately after a successful pop. Both threads read
// the same invariant TSC, so cross-core differences are meaningful.
//
// Both stamps use lfence+rdtsc. A plain rdtsc is not ordered with earlier loads, so
// the consumer's read could execute before the pop's loads and produce a negative
// delta. Any residual negative delta (e.g. hypervisor TSC skew between vCPUs) is
// clamped to zero and counted in the report rather than silently wrapping.
//
// Paced load (--interval-ns > 0) measures the intrinsic hand-off cost. With
// --interval-ns=0 the producer saturates the queue and the samples are dominated by
// queueing delay instead.
//
// Usage:
//   bench_latency --queue=spsc|mutex [--samples=5000000] [--warmup=500000]
//                 [--interval-ns=1000] [--producer-cpu=2] [--consumer-cpu=4]
//                 [--label=name] [--csv=summary.csv] [--curve=percentiles.csv]
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "spsc/cpu_relax.hpp"
#include "spsc/mutex_queue.hpp"
#include "spsc/order_event.hpp"
#include "spsc/spsc_queue.hpp"

namespace {

constexpr std::size_t kCapacity = 65536;

struct Config {
    std::string queue;
    std::string label;
    std::uint64_t samples;
    std::uint64_t warmup;
    std::uint64_t interval_ns;
    int producer_cpu;
    int consumer_cpu;
};

struct RunOutput {
    std::vector<std::uint64_t> samples;
    std::uint64_t negative_deltas;
};

template <typename Queue>
RunOutput run(const Config& cfg, const bench::Environment& env) {
    auto q = std::make_unique<Queue>();
    const std::uint64_t total = cfg.warmup + cfg.samples;
    const auto interval_cycles =
        static_cast<std::uint64_t>(static_cast<double>(cfg.interval_ns) * env.tsc_ghz);
    std::vector<std::uint64_t> samples(cfg.samples);  // value-initialized: pages touched up front
    std::uint64_t checksum = 0;
    std::uint64_t negative_deltas = 0;

    std::thread consumer([&] {
        platform::pin_current_thread(cfg.consumer_cpu);
        spsc::OrderEvent ev{};
        for (std::uint64_t i = 0; i < total; ++i) {
            while (!q->try_pop(ev)) spsc::cpu_relax();
            const std::uint64_t now = platform::rdtsc_ordered();
            if (i >= cfg.warmup) {
                std::uint64_t delta = 0;
                if (now >= ev.tsc_push) {
                    delta = now - ev.tsc_push;
                } else {
                    ++negative_deltas;
                }
                samples[i - cfg.warmup] = delta;
            }
            checksum += ev.seq;
        }
    });
    std::thread producer([&] {
        platform::pin_current_thread(cfg.producer_cpu);
        spsc::OrderEvent ev{};
        std::uint64_t next_send = platform::rdtsc();
        for (std::uint64_t i = 0; i < total; ++i) {
            if (interval_cycles != 0) {
                while (platform::rdtsc() < next_send) spsc::cpu_relax();
                next_send += interval_cycles;
            }
            ev.seq = i;
            ev.tsc_push = platform::rdtsc_ordered();
            while (!q->try_push(ev)) spsc::cpu_relax();
        }
    });
    producer.join();
    consumer.join();

    const std::uint64_t expected = total * (total - 1) / 2;
    if (checksum != expected) {
        std::fprintf(stderr, "checksum mismatch: %" PRIu64 " != %" PRIu64 "\n", checksum, expected);
        std::exit(1);
    }
    return {std::move(samples), negative_deltas};
}

}  // namespace

int main(int argc, char** argv) {
    const bench::Args args(argc, argv);
    Config cfg{
        args.get("queue", "spsc"),
        args.get("label", ""),
        args.get_u64("samples", 5'000'000),
        args.get_u64("warmup", 500'000),
        args.get_u64("interval-ns", 1000),
        args.get_int("producer-cpu", 2),
        args.get_int("consumer-cpu", 4),
    };
    if (cfg.label.empty()) cfg.label = cfg.queue;

    const bench::Environment env = bench::probe_environment();
    bench::print_environment(env);

    RunOutput out;
    if (cfg.queue == "spsc") {
        out = run<spsc::SpscQueue<spsc::OrderEvent, kCapacity>>(cfg, env);
    } else if (cfg.queue == "mutex") {
        out = run<spsc::MutexQueue<spsc::OrderEvent, kCapacity>>(cfg, env);
    } else {
        std::fprintf(stderr, "unknown --queue=%s (expected spsc|mutex)\n", cfg.queue.c_str());
        return 2;
    }
    std::vector<std::uint64_t>& samples = out.samples;

    const bench::LatencySummary s = bench::summarize(samples, env.tsc_ghz);
    std::printf("%-12s samples=%" PRIu64 " interval=%" PRIu64 "ns cpus=%d,%d | "
                "p50=%.0f p90=%.0f p99=%.0f p99.9=%.0f p99.99=%.0f max=%.0f (ns) negative=%" PRIu64 "\n",
                cfg.label.c_str(), cfg.samples, cfg.interval_ns, cfg.producer_cpu,
                cfg.consumer_cpu, s.p50, s.p90, s.p99, s.p999, s.p9999, s.max, out.negative_deltas);

    char row[512];
    std::snprintf(row, sizeof(row),
                  "%s,%s,%" PRIu64 ",%" PRIu64 ",%d,%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%" PRIu64,
                  cfg.label.c_str(), cfg.queue.c_str(), cfg.samples, cfg.interval_ns,
                  cfg.producer_cpu, cfg.consumer_cpu, s.p50, s.p90, s.p99, s.p999, s.p9999, s.max,
                  out.negative_deltas);
    bench::append_csv_row(args.get("csv", ""),
                          "label,queue,samples,interval_ns,producer_cpu,consumer_cpu,"
                          "p50_ns,p90_ns,p99_ns,p999_ns,p9999_ns,max_ns,negative_deltas",
                          row);
    const std::string curve = args.get("curve", "");
    if (!curve.empty()) bench::write_percentile_csv(curve, samples, env.tsc_ghz);
    return 0;
}
