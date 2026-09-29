// Sustained throughput with a saturating producer.
//
// The producer pushes as fast as possible until told to stop; the consumer pops and
// accumulates a checksum that is verified at the end (this also prevents the compiler
// from eliding the work). One warm-up repetition is run and discarded.
//
// Usage:
//   bench_throughput [--queue=spsc] [--seconds=3] [--reps=5] [--empty-backoff=1]
//                    [--producer-cpu=2] [--consumer-cpu=4] [--label=name] [--csv=out.csv]
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
#include "spsc/cpu_relax.hpp"
#include "spsc/order_event.hpp"

namespace {

struct RunResult {
    double ops_per_sec;
    double consumer_cpu_util;  // consumer CPU time / wall time
    double full_retries;       // failed try_push calls per event (queue observed full)
    double empty_retries;      // failed try_pop calls per event (queue observed empty)
    double mean_depth;         // mean occupancy, sampled by the consumer every 4096 pops
};

template <typename Queue>
RunResult run_once(double seconds, int producer_cpu, int consumer_cpu, std::uint32_t empty_backoff) {
    using clock = std::chrono::steady_clock;
    auto q = std::make_unique<Queue>();
    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<bool> producer_done{false};
    std::uint64_t produced = 0, consumed = 0, checksum = 0;
    std::uint64_t push_fails = 0, pop_fails = 0, depth_sum = 0, depth_samples = 0;
    double consumer_cpu_seconds = 0.0;
    clock::time_point t_end;

    std::thread consumer([&] {
        platform::pin_current_thread(consumer_cpu);
        while (!go.load(std::memory_order_acquire)) spsc::cpu_relax();
        const double cpu0 = platform::thread_cpu_seconds();
        spsc::OrderEvent ev{};
        for (;;) {
            if (q->try_pop(ev)) {
                ++consumed;
                checksum += ev.seq;
                if ((consumed & 4095) == 0) {
                    depth_sum += q->size_approx();
                    ++depth_samples;
                }
            } else if (producer_done.load(std::memory_order_acquire)) {
                while (q->try_pop(ev)) {
                    ++consumed;
                    checksum += ev.seq;
                }
                break;
            } else {
                ++pop_fails;
                // Back off before re-reading the producer's index: each re-read pulls
                // head_'s cache line away from the producer.
                for (std::uint32_t i = 0; i < empty_backoff; ++i) spsc::cpu_relax();
            }
        }
        t_end = clock::now();
        consumer_cpu_seconds = platform::thread_cpu_seconds() - cpu0;
    });
    std::thread producer([&] {
        platform::pin_current_thread(producer_cpu);
        while (!go.load(std::memory_order_acquire)) spsc::cpu_relax();
        spsc::OrderEvent ev{};
        std::uint64_t seq = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            ev.seq = seq;
            if (q->try_push(ev)) {
                ++seq;
            } else {
                ++push_fails;
                spsc::cpu_relax();
            }
        }
        produced = seq;
        producer_done.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let both threads pin and spin
    const auto t_start = clock::now();
    go.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    stop.store(true, std::memory_order_relaxed);
    producer.join();
    consumer.join();

    const std::uint64_t expected = produced == 0 ? 0 : produced * (produced - 1) / 2;
    if (consumed != produced || checksum != expected) {
        std::fprintf(stderr, "verification failed: produced=%" PRIu64 " consumed=%" PRIu64 "\n",
                     produced, consumed);
        std::exit(1);
    }
    const double wall = std::chrono::duration<double>(t_end - t_start).count();
    const double n = static_cast<double>(consumed);
    return {n / wall, consumer_cpu_seconds / wall, static_cast<double>(push_fails) / n,
            static_cast<double>(pop_fails) / n,
            depth_samples ? static_cast<double>(depth_sum) / static_cast<double>(depth_samples) : 0.0};
}

template <typename Queue>
std::vector<RunResult> run(double seconds, int reps, int producer_cpu, int consumer_cpu,
                           std::uint32_t empty_backoff) {
    run_once<Queue>(std::min(seconds, 1.0), producer_cpu, consumer_cpu, empty_backoff);  // warm-up
    std::vector<RunResult> results;
    for (int i = 0; i < reps; ++i) {
        results.push_back(run_once<Queue>(seconds, producer_cpu, consumer_cpu, empty_backoff));
    }
    return results;
}

}  // namespace

int main(int argc, char** argv) {
    const bench::Args args(argc, argv);
    const std::string queue = args.get("queue", "spsc");
    std::string label = args.get("label", "");
    if (label.empty()) label = queue;
    const double seconds = args.get_double("seconds", 3.0);
    const int reps = args.get_int("reps", 5);
    const int producer_cpu = args.get_int("producer-cpu", 2);
    const int consumer_cpu = args.get_int("consumer-cpu", 4);
    const auto empty_backoff = static_cast<std::uint32_t>(args.get_u64("empty-backoff", 1));

    std::vector<RunResult> results;
    const bool ok = bench::with_queue(queue, [&]<typename Queue>() {
        results = run<Queue>(seconds, reps, producer_cpu, consumer_cpu, empty_backoff);
    });
    if (!ok || results.empty()) return 2;

    std::sort(results.begin(), results.end(),
              [](const RunResult& a, const RunResult& b) { return a.ops_per_sec < b.ops_per_sec; });
    const RunResult& med = results[results.size() / 2];  // diagnostics come from the median run
    const double lo = results.front().ops_per_sec, hi = results.back().ops_per_sec;

    std::printf("%-12s reps=%d seconds=%.1f cpus=%d,%d | median=%.2f Mops/s min=%.2f max=%.2f "
                "consumer_cpu=%.0f%% | full_retries/op=%.3f empty_retries/op=%.3f mean_depth=%.0f\n",
                label.c_str(), reps, seconds, producer_cpu, consumer_cpu, med.ops_per_sec / 1e6,
                lo / 1e6, hi / 1e6, med.consumer_cpu_util * 100.0, med.full_retries,
                med.empty_retries, med.mean_depth);

    char row[384];
    std::snprintf(row, sizeof(row), "%s,%s,%d,%.1f,%d,%d,%.0f,%.0f,%.0f,%.3f,%.4f,%.4f,%.1f",
                  label.c_str(), queue.c_str(), reps, seconds, producer_cpu, consumer_cpu,
                  med.ops_per_sec, lo, hi, med.consumer_cpu_util, med.full_retries,
                  med.empty_retries, med.mean_depth);
    bench::append_csv_row(args.get("csv", ""),
                          "label,queue,reps,seconds,producer_cpu,consumer_cpu,"
                          "median_ops_per_sec,min_ops_per_sec,max_ops_per_sec,consumer_cpu_util,"
                          "full_retries_per_op,empty_retries_per_op,mean_depth",
                          row);
    return 0;
}
