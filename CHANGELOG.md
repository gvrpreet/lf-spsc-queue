# Changelog

## Unreleased

### Added
- `examples/lob_replay`: two-thread market replay on [lob-engine](https://github.com/gvrpreet/lob-engine).
  A reader thread feeds its order book through the SPSC queue, and the final book checksum and
  every counter are verified against lob-engine's single-threaded replay: identical on a full
  Nasdaq AAPL day (2.0M events) and on 50M synthetic events. Enabled with
  `-DSPSC_LOB_ENGINE_DIR=<checkout>` or `-DSPSC_FETCH_LOB_ENGINE=ON`.
- GitHub Actions CI: debug, release, AddressSanitizer, and ThreadSanitizer builds with GCC and
  Clang, plus a mutation-testing smoke job.
- `.gitattributes`, `.editorconfig`.
- Results: wait-strategy latency and CPU at three loads, the lob-engine replay, and a
  consumer back-off sweep testing the explanation for the `seq_cst` throughput result.

## 0.2.0 - 2026-09-29

### Added
- Compile-time `Tuning` for the queue: cache-line isolation of the indices (64 or 128 bytes),
  cached remote indices (now the default), consumer prefetch, and `seq_cst` for comparison.
- Wait strategies: busy-spin, spin-then-yield, and spin-then-park (futex) with a
  lost-wake-up-safe protocol.
- Bursty-load benchmark measuring latency from the intended send time.
- Order-pipeline example with a price-time-priority order book.
- Experiment runner (`scripts/run_experiments.sh`), dependency-free SVG charts, and results for
  ablations, bursty load, and the pipeline.
- Mutation testing for the cached-index path and the park protocol (15 mutations).

## 0.1.0 - 2026-09-28

### Added
- Lock-free SPSC ring buffer with monotonic 64-bit indices and acquire/release ordering.
- Full-queue backpressure policy: bounded spin, yield, explicit reject.
- `std::mutex` + `std::queue` baseline, latency and throughput benchmarks.
- Unit, stress, and sanitizer test suites; mutation-testing harness.
- MIT license.
