// Bursty-load benchmark (planned).
//
// Design:
//   - precomputed on/off schedule: bursts of B events at maximum rate followed by
//     idle gaps of G, plus a Poisson-arrival mode
//   - latency measured from the intended send time in the schedule rather than the
//     actual push time, which avoids coordinated omission
//   - producer uses spsc::push_with_backpressure and reports PushStats
//     (full events, rejections, queue-depth high-water mark)
//   - identical schedules for SpscQueue and MutexQueue
#include <cstdio>

#include "bench_common.hpp"
#include "spsc/backpressure.hpp"
#include "spsc/mutex_queue.hpp"
#include "spsc/order_event.hpp"
#include "spsc/spsc_queue.hpp"

int main() {
    std::puts("bench_burst: not implemented yet");
    return 0;
}
