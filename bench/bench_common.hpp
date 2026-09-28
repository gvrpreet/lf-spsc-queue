#pragma once

// Shared benchmark utilities: argument parsing, percentile statistics, CSV output.
// Timing and pinning primitives live in common/platform.hpp.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "platform.hpp"

namespace bench {

// Minimal --key=value parser.
class Args {
public:
    Args(int argc, char** argv) : argv_(argv + 1, argv + argc) {}

    std::string get(std::string_view key, std::string fallback) const {
        for (const std::string& a : argv_) {
            if (a.size() > key.size() + 3 && a.compare(0, 2, "--") == 0 &&
                a.compare(2, key.size(), key) == 0 && a[key.size() + 2] == '=') {
                return a.substr(key.size() + 3);
            }
        }
        return fallback;
    }
    std::uint64_t get_u64(std::string_view key, std::uint64_t fallback) const {
        const std::string v = get(key, "");
        return v.empty() ? fallback : std::strtoull(v.c_str(), nullptr, 10);
    }
    int get_int(std::string_view key, int fallback) const {
        const std::string v = get(key, "");
        return v.empty() ? fallback : std::atoi(v.c_str());
    }
    double get_double(std::string_view key, double fallback) const {
        const std::string v = get(key, "");
        return v.empty() ? fallback : std::strtod(v.c_str(), nullptr);
    }

private:
    std::vector<std::string> argv_;
};

struct Environment {
    double tsc_ghz = 0.0;
    bool invariant_tsc = false;
    std::uint64_t rdtsc_min_delta = 0;    // back-to-back plain rdtsc (may overlap out of order)
    std::uint64_t rdtsc_ordered_cost = 0; // back-to-back lfence+rdtsc (serialized cost)
};

// Minimum over many back-to-back reads.
template <typename ReadFn>
std::uint64_t min_back_to_back(ReadFn read) {
    std::uint64_t best = ~0ull;
    for (int i = 0; i < 1'000'000; ++i) {
        const std::uint64_t a = read();
        const std::uint64_t b = read();
        best = std::min(best, b - a);
    }
    return best;
}

inline Environment probe_environment() {
    Environment env;
    env.invariant_tsc = platform::has_invariant_tsc();
    env.tsc_ghz = platform::calibrate_tsc_ghz();
    env.rdtsc_min_delta = min_back_to_back(platform::rdtsc);
    env.rdtsc_ordered_cost = min_back_to_back(platform::rdtsc_ordered);
    return env;
}

inline void print_environment(const Environment& env) {
    std::printf("# tsc: %.3f GHz, invariant=%s, rdtsc min delta=%llu cycles, "
                "lfence+rdtsc=%llu cycles (%.1f ns)\n",
                env.tsc_ghz, env.invariant_tsc ? "yes" : "NO",
                static_cast<unsigned long long>(env.rdtsc_min_delta),
                static_cast<unsigned long long>(env.rdtsc_ordered_cost),
                static_cast<double>(env.rdtsc_ordered_cost) / env.tsc_ghz);
}

// Nearest-rank percentile over a sorted sample vector.
inline std::uint64_t percentile_sorted(const std::vector<std::uint64_t>& sorted, double p) {
    if (sorted.empty()) return 0;
    const double rank = p / 100.0 * static_cast<double>(sorted.size());
    std::size_t idx = static_cast<std::size_t>(rank);
    if (idx >= sorted.size()) idx = sorted.size() - 1;
    return sorted[idx];
}

struct LatencySummary {
    double p50, p90, p99, p999, p9999, max;  // nanoseconds
};

// Sorts `cycles` in place.
inline LatencySummary summarize(std::vector<std::uint64_t>& cycles, double tsc_ghz) {
    std::sort(cycles.begin(), cycles.end());
    auto ns = [&](double p) { return static_cast<double>(percentile_sorted(cycles, p)) / tsc_ghz; };
    return {ns(50), ns(90), ns(99), ns(99.9), ns(99.99),
            cycles.empty() ? 0.0 : static_cast<double>(cycles.back()) / tsc_ghz};
}

// Percentile curve for HDR-style plots: one row per percentile, on a "nines" scale.
inline void write_percentile_csv(const std::string& path, const std::vector<std::uint64_t>& sorted,
                                 double tsc_ghz) {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        std::perror(path.c_str());
        return;
    }
    std::fprintf(f, "percentile,latency_ns\n");
    static constexpr double kPercentiles[] = {
        0,    10,   20,    25,    30,     40,     50,      60,      70,       75,
        80,   90,   95,    97,    98,     99,     99.5,    99.7,    99.9,     99.95,
        99.97, 99.99, 99.995, 99.999, 99.9995, 99.9999, 100};
    for (double p : kPercentiles) {
        std::fprintf(f, "%g,%.1f\n", p, static_cast<double>(percentile_sorted(sorted, p)) / tsc_ghz);
    }
    std::fclose(f);
}

// Appends one row to a CSV, writing the header if the file is new/empty.
inline void append_csv_row(const std::string& path, const std::string& header,
                           const std::string& row) {
    if (path.empty()) return;
    bool need_header = true;
    if (std::FILE* probe = std::fopen(path.c_str(), "r")) {
        need_header = std::fgetc(probe) == EOF;
        std::fclose(probe);
    }
    std::FILE* f = std::fopen(path.c_str(), "a");
    if (!f) {
        std::perror(path.c_str());
        return;
    }
    if (need_header) std::fprintf(f, "%s\n", header.c_str());
    std::fprintf(f, "%s\n", row.c_str());
    std::fclose(f);
}

}  // namespace bench
