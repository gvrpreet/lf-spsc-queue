// Order pipeline example (planned): intake thread -> SpscQueue -> matching thread.
//
// Design:
//   Intake thread (producer), pinned:
//     - synthetic order generator (new/cancel/modify mix, configurable rate)
//     - validation (qty > 0, price within band, known instrument); invalid
//       requests are rejected at intake and never enqueued
//     - builds OrderEvent (tsc_intake, tsc_push) -> push_with_backpressure
//   Matching thread (consumer), pinned:
//     - owns the order book exclusively (no locks): price levels -> FIFO of orders
//     - applies New/Cancel/Modify, generates trades/acks
//     - records end-to-end latency: ack time - tsc_intake
//   Shutdown: the producer sets a done flag after its last push; the consumer drains.
#include <cstdio>

#include "spsc/backpressure.hpp"
#include "spsc/order_event.hpp"
#include "spsc/spsc_queue.hpp"
#include "spsc/wait_strategy.hpp"

int main() {
    std::puts("order_pipeline: not implemented yet");
    return 0;
}
