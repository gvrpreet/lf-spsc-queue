// Sustained throughput with a saturating producer.
//
// The producer pushes as fast as possible until told to stop; the consumer pops and
// accumulates a checksum that is verified at the end (this also prevents the compiler
// from eliding the work). One warm-up repetition is run and discarded.
//
// Usage:
//   bench_throughput --queue=spsc|mutex [--seconds=3] [--reps=5]
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
#include "spsc/cpu_relax.hpp"
#include "spsc/mutex_queue.hpp"
#include "spsc/order_event.hpp"
#include "spsc/spsc_queue.hpp"

namespace {

constexpr std::size_t kCapacity = 65536;

struct RunResult {
    double ops_per_sec;
    double consumer_cpu_util;  // consumer CPU time / wall time
};

template <typename Queue>
RunResult run_once(double seconds, int producer_cpu, int consumer_cpu) {
    using clock = std::chrono::steady_clock;
    auto q = std::make_unique<Queue>();
    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<bool> producer_done{false};
    std::uint64_t produced = 0, consumed = 0, checksum = 0;
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
            } else if (producer_done.load(std::memory_order_acquire)) {
                while (q->try_pop(ev)) {
                    ++consumed;
                    checksum += ev.seq;
                }
                break;
            } else {
                spsc::cpu_relax();
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
    return {static_cast<double>(consumed) / wall, consumer_cpu_seconds / wall};
}

template <typename Queue>
std::vector<RunResult> run(double seconds, int reps, int producer_cpu, int consumer_cpu) {
    run_once<Queue>(std::min(seconds, 1.0), producer_cpu, consumer_cpu);  // warm-up, discarded
    std::vector<RunResult> results;
    for (int i = 0; i < reps; ++i) results.push_back(run_once<Queue>(seconds, producer_cpu, consumer_cpu));
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

    std::vector<RunResult> results;
    if (queue == "spsc") {
        results = run<spsc::SpscQueue<spsc::OrderEvent, kCapacity>>(seconds, reps, producer_cpu, consumer_cpu);
    } else if (queue == "mutex") {
        results = run<spsc::MutexQueue<spsc::OrderEvent, kCapacity>>(seconds, reps, producer_cpu, consumer_cpu);
    } else {
        std::fprintf(stderr, "unknown --queue=%s (expected spsc|mutex)\n", queue.c_str());
        return 2;
    }

    std::vector<double> ops;
    for (const RunResult& r : results) ops.push_back(r.ops_per_sec);
    std::sort(ops.begin(), ops.end());
    const double median = ops[ops.size() / 2];
    const double cpu = results.back().consumer_cpu_util;

    std::printf("%-12s reps=%d seconds=%.1f cpus=%d,%d | median=%.2f Mops/s min=%.2f max=%.2f "
                "consumer_cpu=%.0f%%\n",
                label.c_str(), reps, seconds, producer_cpu, consumer_cpu, median / 1e6,
                ops.front() / 1e6, ops.back() / 1e6, cpu * 100.0);

    char row[256];
    std::snprintf(row, sizeof(row), "%s,%s,%d,%.1f,%d,%d,%.0f,%.0f,%.0f,%.3f", label.c_str(),
                  queue.c_str(), reps, seconds, producer_cpu, consumer_cpu, median, ops.front(),
                  ops.back(), cpu);
    bench::append_csv_row(args.get("csv", ""),
                          "label,queue,reps,seconds,producer_cpu,consumer_cpu,"
                          "median_ops_per_sec,min_ops_per_sec,max_ops_per_sec,consumer_cpu_util",
                          row);
    return 0;
}
