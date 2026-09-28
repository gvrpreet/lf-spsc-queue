#!/usr/bin/env python3
"""Plot benchmark CSVs from docs/results/ (planned).

Planned output:
  - percentile plot (x: percentile on a "nines" scale 50 .. 99.99, y: latency ns),
    one line per queue, from bench_latency --curve output
  - throughput bar chart with min/max whiskers across repetitions
  - ablation chart: change relative to baseline A0
Dependencies: matplotlib, pandas (pip install -r scripts/requirements.txt).
"""


def main() -> None:
    raise SystemExit("plot_results.py: not implemented yet")


if __name__ == "__main__":
    main()
