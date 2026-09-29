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
//   bench_latency [--queue=spsc] [--wait=spin|yield|park] [--spin=1024] [--samples=5000000]
//                 [--warmup=500000] [--interval-ns=1000] [--producer-cpu=2]
//                 [--consumer-cpu=4] [--label=name] [--csv=summary.csv]
//                 [--curve=percentiles.csv]
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "bench_common.hpp"
#include "queue_variants.hpp"
#include "spsc/cpu_relax.hpp"
#include "spsc/order_event.hpp"

namespace {

struct Config {
    std::string queue;
    std::string wait;
    std::string label;
    std::uint64_t samples;
    std::uint64_t warmup;
    std::uint64_t interval_ns;
    int producer_cpu;
    int consumer_cpu;
    std::uint32_t spin;  // spin budget for yield/park before they give up the CPU
};

struct RunOutput {
    std::vector<std::uint64_t> samples;
    std::uint64_t negative_deltas = 0;
    double consumer_cpu_util = 0.0;  // consumer thread CPU time / wall time
};

template <typename Wait>
std::unique_ptr<Wait> make_wait(std::uint32_t spin) {
    if constexpr (std::is_same_v<Wait, spsc::BusySpin>) {
        return std::make_unique<Wait>();
    } else {
        return std::make_unique<Wait>(spin);
    }
}

template <typename Queue, typename Wait>
RunOutput run(const Config& cfg, const bench::Environment& env) {
    using clock = std::chrono::steady_clock;
    auto q = std::make_unique<Queue>();
    auto wait = make_wait<Wait>(cfg.spin);
    const std::uint64_t total = cfg.warmup + cfg.samples;
    const auto interval_cycles =
        static_cast<std::uint64_t>(static_cast<double>(cfg.interval_ns) * env.tsc_ghz);

    RunOutput out;
    out.samples.resize(cfg.samples);  // value-initialized: pages touched up front
    std::uint64_t checksum = 0;

    std::thread consumer([&] {
        platform::pin_current_thread(cfg.consumer_cpu);
        const double cpu0 = platform::thread_cpu_seconds();
        const auto wall0 = clock::now();
        spsc::OrderEvent ev{};
        for (std::uint64_t i = 0; i < total; ++i) {
            wait->pop(*q, ev);
            const std::uint64_t now = platform::rdtsc_ordered();
            if (i >= cfg.warmup) {
                std::uint64_t delta = 0;
                if (now >= ev.tsc_push) {
                    delta = now - ev.tsc_push;
                } else {
                    ++out.negative_deltas;
                }
                out.samples[i - cfg.warmup] = delta;
            }
            checksum += ev.seq;
        }
        const double wall = std::chrono::duration<double>(clock::now() - wall0).count();
        out.consumer_cpu_util = (platform::thread_cpu_seconds() - cpu0) / wall;
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
            wait->notify();
        }
    });
    producer.join();
    consumer.join();

    const std::uint64_t expected = total * (total - 1) / 2;
    if (checksum != expected) {
        std::fprintf(stderr, "checksum mismatch: %" PRIu64 " != %" PRIu64 "\n", checksum, expected);
        std::exit(1);
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    const bench::Args args(argc, argv);
    Config cfg{
        args.get("queue", "spsc"),
        args.get("wait", "spin"),
        args.get("label", ""),
        args.get_u64("samples", 5'000'000),
        args.get_u64("warmup", 500'000),
        args.get_u64("interval-ns", 1000),
        args.get_int("producer-cpu", 2),
        args.get_int("consumer-cpu", 4),
        static_cast<std::uint32_t>(args.get_u64("spin", 1024)),
    };
    if (cfg.label.empty()) cfg.label = cfg.queue + (cfg.wait == "spin" ? "" : "+" + cfg.wait);

    const bench::Environment env = bench::probe_environment();
    bench::print_environment(env);

    RunOutput out;
    const bool ok = bench::with_queue(cfg.queue, [&]<typename Queue>() {
        bench::with_wait(cfg.wait, [&]<typename Wait>() { out = run<Queue, Wait>(cfg, env); });
    });
    if (!ok || out.samples.empty()) return 2;

    std::vector<std::uint64_t>& samples = out.samples;
    const bench::LatencySummary s = bench::summarize(samples, env.tsc_ghz);
    std::printf("%-16s interval=%" PRIu64 "ns cpus=%d,%d | p50=%.0f p90=%.0f p99=%.0f "
                "p99.9=%.0f p99.99=%.0f max=%.0f (ns) consumer_cpu=%.0f%% negative=%" PRIu64 "\n",
                cfg.label.c_str(), cfg.interval_ns, cfg.producer_cpu, cfg.consumer_cpu, s.p50,
                s.p90, s.p99, s.p999, s.p9999, s.max, out.consumer_cpu_util * 100.0,
                out.negative_deltas);

    char row[512];
    std::snprintf(row, sizeof(row),
                  "%s,%s,%s,%" PRIu64 ",%" PRIu64 ",%d,%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.3f,%" PRIu64,
                  cfg.label.c_str(), cfg.queue.c_str(), cfg.wait.c_str(), cfg.samples,
                  cfg.interval_ns, cfg.producer_cpu, cfg.consumer_cpu, s.p50, s.p90, s.p99,
                  s.p999, s.p9999, s.max, out.consumer_cpu_util, out.negative_deltas);
    bench::append_csv_row(args.get("csv", ""),
                          "label,queue,wait,samples,interval_ns,producer_cpu,consumer_cpu,"
                          "p50_ns,p90_ns,p99_ns,p999_ns,p9999_ns,max_ns,consumer_cpu_util,"
                          "negative_deltas",
                          row);
    const std::string curve = args.get("curve", "");
    if (!curve.empty()) bench::write_percentile_csv(curve, samples, env.tsc_ghz);
    return 0;
}
