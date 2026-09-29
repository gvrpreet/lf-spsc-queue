#pragma once

// Maps --queue=<name> to a concrete queue type so every benchmark can compare the
// same set of configurations. IDs match the experiment table in docs/design/DESIGN.md §7.

#include <cstddef>
#include <cstdio>
#include <string>

#include "spsc/mutex_queue.hpp"
#include "spsc/order_event.hpp"
#include "spsc/spsc_queue.hpp"
#include "spsc/wait_strategy.hpp"

namespace bench {

inline constexpr std::size_t kBenchCapacity = 65536;

template <spsc::Tuning Tune>
using Spsc = spsc::SpscQueue<spsc::OrderEvent, kBenchCapacity, Tune>;

inline constexpr const char* kQueueNames =
    "mutex | a0 (unpadded, uncached) | a1 (padded) | spsc (padded + cached, default) | "
    "pad128 | prefetch | seqcst";

// Invokes fn.template operator()<QueueType>() for the named queue. Returns false if
// the name is unknown.
template <typename Fn>
bool with_queue(const std::string& name, Fn&& fn) {
    if (name == "mutex") {
        fn.template operator()<spsc::MutexQueue<spsc::OrderEvent, kBenchCapacity>>();
    } else if (name == "a0") {
        fn.template operator()<Spsc<spsc::kBaselineTuning>>();
    } else if (name == "a1") {
        fn.template operator()<Spsc<spsc::Tuning{.index_align = 64, .cache_remote_index = false}>>();
    } else if (name == "spsc" || name == "a2") {
        fn.template operator()<Spsc<spsc::Tuning{}>>();
    } else if (name == "pad128") {
        fn.template operator()<Spsc<spsc::Tuning{.index_align = 128}>>();
    } else if (name == "prefetch") {
        fn.template operator()<Spsc<spsc::Tuning{.prefetch_next = true}>>();
    } else if (name == "seqcst") {
        fn.template operator()<Spsc<spsc::Tuning{.seq_cst = true}>>();
    } else {
        std::fprintf(stderr, "unknown --queue=%s (expected %s)\n", name.c_str(), kQueueNames);
        return false;
    }
    return true;
}

// Same idea for --wait=spin|yield|park.
template <typename Fn>
bool with_wait(const std::string& name, Fn&& fn) {
    if (name == "spin") {
        fn.template operator()<spsc::BusySpin>();
    } else if (name == "yield") {
        fn.template operator()<spsc::SpinThenYield>();
    } else if (name == "park") {
        fn.template operator()<spsc::SpinThenPark>();
    } else {
        std::fprintf(stderr, "unknown --wait=%s (expected spin | yield | park)\n", name.c_str());
        return false;
    }
    return true;
}

}  // namespace bench
