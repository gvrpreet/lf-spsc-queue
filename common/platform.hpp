#pragma once

// Platform helpers shared by tests, benches, and examples (not part of the queue).

#include <pthread.h>
#include <sched.h>

#include <chrono>
#include <cstdint>
#include <thread>

#if defined(__x86_64__)
#include <cpuid.h>
#include <x86intrin.h>
#endif

namespace platform {

// Raw TSC read. Not serializing: may execute before earlier instructions finish.
inline std::uint64_t rdtsc() noexcept { return __rdtsc(); }

// lfence before rdtsc: all earlier instructions have completed locally before the
// timestamp is taken. Use at the boundaries of short timed regions.
inline std::uint64_t rdtsc_ordered() noexcept {
    _mm_lfence();
    return __rdtsc();
}

// CPUID 0x80000007 EDX bit 8: TSC runs at a constant rate in all P/C-states and is
// synchronized across cores, so cross-core TSC differences are meaningful.
inline bool has_invariant_tsc() noexcept {
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (!__get_cpuid(0x80000007, &eax, &ebx, &ecx, &edx)) return false;
    return (edx >> 8) & 1u;
}

// TSC ticks per nanosecond, measured against steady_clock over `window`.
inline double calibrate_tsc_ghz(std::chrono::milliseconds window = std::chrono::milliseconds(200)) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const std::uint64_t c0 = rdtsc_ordered();
    while (clock::now() - t0 < window) {
    }
    const std::uint64_t c1 = rdtsc_ordered();
    const auto t1 = clock::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    return static_cast<double>(c1 - c0) / ns;
}

// Pins the calling thread to one logical CPU. Returns false on failure.
inline bool pin_current_thread(int cpu) noexcept {
    if (cpu < 0) return true;  // negative = unpinned
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

// Saves the calling thread's CPU affinity and restores it on destruction.
class AffinityGuard {
public:
    AffinityGuard() noexcept { pthread_getaffinity_np(pthread_self(), sizeof(saved_), &saved_); }
    ~AffinityGuard() { pthread_setaffinity_np(pthread_self(), sizeof(saved_), &saved_); }
    AffinityGuard(const AffinityGuard&) = delete;
    AffinityGuard& operator=(const AffinityGuard&) = delete;

private:
    cpu_set_t saved_{};
};

inline double thread_cpu_seconds() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

}  // namespace platform
