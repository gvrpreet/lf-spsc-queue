#!/usr/bin/env python3
"""Render benchmark results as standalone SVG charts (standard library only).

Usage:
  scripts/plot_results.py <ablations-dir> [--out docs/results/plots]

Reads the output of `scripts/run_experiments.sh ablations`:
  curve_<queue>_run<k>.csv   latency percentile curves (median taken across runs)
  throughput.csv             throughput median/min/max per configuration

Writes:
  latency_percentiles.svg    push->pop latency vs. percentile, log scale
  throughput.svg             sustained throughput per configuration with min-max range

Charts follow light/dark mode via prefers-color-scheme. Colors are the first three
categorical slots of a CVD-validated palette (valid for all pairs in both modes).
"""

from __future__ import annotations

import argparse
import csv
import math
import statistics
from pathlib import Path

FONT = 'system-ui, -apple-system, "Segoe UI", sans-serif'

STYLE = """
<style>
  svg { --surface:#fcfcfb; --ink:#0b0b0b; --ink2:#52514e; --muted:#898781;
        --grid:#e1e0d9; --axis:#c3c2b7; --s1:#2a78d6; --s2:#eb6834; --s3:#1baf7a; }
  @media (prefers-color-scheme: dark) {
    svg { --surface:#1a1a19; --ink:#ffffff; --ink2:#c3c2b7; --muted:#898781;
          --grid:#2c2c2a; --axis:#383835; --s1:#3987e5; --s2:#d95926; --s3:#199e70; }
  }
  .bg { fill: var(--surface); }
  .title { fill: var(--ink); font: 600 15px %(font)s; }
  .subtitle { fill: var(--ink2); font: 12px %(font)s; }
  .tick { fill: var(--muted); font: 11px %(font)s; font-variant-numeric: tabular-nums; }
  .label { fill: var(--ink2); font: 12px %(font)s; }
  .value { fill: var(--ink); font: 12px %(font)s; font-variant-numeric: tabular-nums; }
  .grid { stroke: var(--grid); stroke-width: 1; }
  .axis { stroke: var(--axis); stroke-width: 1; }
  .line { fill: none; stroke-width: 2; stroke-linejoin: round; stroke-linecap: round; }
  .whisker { stroke: var(--ink2); stroke-width: 1.5; }
</style>
""" % {"font": FONT}


def esc(text: str) -> str:
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def fmt_ns(ns: float) -> str:
    if ns >= 1e6:
        return f"{ns / 1e6:g} ms"
    if ns >= 1e3:
        return f"{ns / 1e3:g} µs"
    return f"{ns:g} ns"


# --------------------------------------------------------------------------- latency


def nines(p: float) -> float:
    """Percentile -> x position on a 'nines' axis: 50->0.3, 90->1, 99->2, 99.9->3."""
    return -math.log10(1.0 - p / 100.0)


def median_curve(files: list[Path]) -> list[tuple[float, float]]:
    runs = [{float(r["percentile"]): float(r["latency_ns"]) for r in read_csv(f)} for f in files]
    points = sorted(set.intersection(*(set(r) for r in runs)))
    return [(p, statistics.median(r[p] for r in runs)) for p in points if 50 <= p <= 99.99]


def latency_chart(ablations: Path, out: Path) -> None:
    series = [  # (queue id in file names, legend label, color slot)
        ("mutex", "std::mutex + std::queue", "--s2"),
        ("a0", "SPSC baseline", "--s3"),
        ("spsc", "SPSC tuned", "--s1"),
    ]
    curves = []
    for qid, label, color in series:
        files = sorted(ablations.glob(f"curve_{qid}_run*.csv"))
        if files:
            curves.append((label, color, median_curve(files)))
    if not curves:
        raise SystemExit(f"no curve_*_run*.csv files in {ablations}")

    w, h = 760, 420
    left, right, top, bottom = 70, 24, 64, 96
    pw, ph = w - left - right, h - top - bottom
    x_min, x_max = nines(50), nines(99.99)
    y_vals = [v for _, _, c in curves for _, v in c if v > 0]
    y_lo = 10 ** math.floor(math.log10(min(y_vals)))
    y_hi = 10 ** math.ceil(math.log10(max(y_vals)))

    def px(p: float) -> float:
        return left + (nines(p) - x_min) / (x_max - x_min) * pw

    def py(v: float) -> float:
        v = max(v, y_lo)
        return top + ph - (math.log10(v) - math.log10(y_lo)) / (math.log10(y_hi) - math.log10(y_lo)) * ph

    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" '
             f'role="img" aria-labelledby="t d">', STYLE,
             f'<rect class="bg" width="{w}" height="{h}" rx="8"/>',
             '<title id="t">Push-to-pop latency by percentile</title>',
             '<desc id="d">Latency percentile curves for the mutex baseline and SPSC queue variants, '
             'median of runs, log scale.</desc>',
             f'<text class="title" x="{left}" y="28">Push-to-pop latency by percentile</text>',
             f'<text class="subtitle" x="{left}" y="46">1 event/µs, separate cores, median of runs. '
             f'Baseline: indices share a cache line. Tuned: padded + cached indices.</text>']

    decade = y_lo
    while decade <= y_hi * 1.0001:
        y = py(decade)
        parts.append(f'<line class="grid" x1="{left}" x2="{left + pw}" y1="{y:.1f}" y2="{y:.1f}"/>')
        parts.append(f'<text class="tick" x="{left - 8}" y="{y + 4:.1f}" text-anchor="end">{fmt_ns(decade)}</text>')
        decade *= 10
    for p, lab in [(50, "p50"), (90, "p90"), (99, "p99"), (99.9, "p99.9"), (99.99, "p99.99")]:
        x = px(p)
        parts.append(f'<line class="grid" x1="{x:.1f}" x2="{x:.1f}" y1="{top}" y2="{top + ph}"/>')
        parts.append(f'<text class="tick" x="{x:.1f}" y="{top + ph + 18}" text-anchor="middle">{lab}</text>')
    parts.append(f'<line class="axis" x1="{left}" x2="{left + pw}" y1="{top + ph}" y2="{top + ph}"/>')

    for label, color, curve in curves:
        d = " ".join(f"{'M' if i == 0 else 'L'}{px(p):.1f},{py(v):.1f}" for i, (p, v) in enumerate(curve))
        parts.append(f'<path class="line" style="stroke:var({color})" d="{d}"/>')
        for p, v in curve:
            if p in (50, 90, 99, 99.9, 99.99):
                parts.append(f'<circle cx="{px(p):.1f}" cy="{py(v):.1f}" r="4" style="fill:var({color})" '
                             f'stroke="var(--surface)" stroke-width="2"><title>{esc(label)} p{p:g}: '
                             f'{fmt_ns(round(v))}</title></circle>')

    # Legend (always present for >= 2 series), below the plot; wraps if it would overflow.
    lx, ly = left, h - 40
    for label, color, _ in curves:
        item_w = 24 + 7.0 * len(label) + 28
        if lx + item_w > w - right:
            lx, ly = left, ly + 20
        parts.append(f'<line x1="{lx}" x2="{lx + 18}" y1="{ly}" y2="{ly}" style="stroke:var({color})" '
                     f'stroke-width="2" stroke-linecap="round"/>')
        parts.append(f'<text class="label" x="{lx + 24}" y="{ly + 4}">{esc(label)}</text>')
        lx += item_w
    parts.append(f'<text class="tick" x="{left + pw}" y="{h - 14}" text-anchor="end">Above p99, '
                 f'samples are dominated by WSL2 vCPU scheduling noise.</text>')
    parts.append("</svg>")
    out.write_text("\n".join(parts), encoding="utf-8")


# ------------------------------------------------------------------------ throughput

THROUGHPUT_LABELS = {
    "mutex": "std::mutex + std::queue",
    "a0": "A0  SPSC baseline (unpadded)",
    "a1": "A1  + index padding",
    "spsc": "A2  + cached indices (default)",
    "pad128": "      128-byte padding",
    "prefetch": "A4  + consumer prefetch",
    "seqcst": "A5  seq_cst everywhere",
    "a0-smt": "A6  baseline on SMT siblings",
    "spsc-smt": "A6  default on SMT siblings",
}


def throughput_chart(ablations: Path, out: Path) -> None:
    rows = read_csv(ablations / "throughput.csv")
    data = [(THROUGHPUT_LABELS.get(r["label"], r["label"]), float(r["median_ops_per_sec"]) / 1e6,
             float(r["min_ops_per_sec"]) / 1e6, float(r["max_ops_per_sec"]) / 1e6) for r in rows]

    bar_h, gap = 22, 10
    w = 760
    left, right, top = 240, 90, 64
    h = top + len(data) * (bar_h + gap) + 40
    pw = w - left - right
    x_max = max(hi for *_, hi in data) * 1.05
    step = 10 if x_max > 60 else 5

    def px(v: float) -> float:
        return left + v / x_max * pw

    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" '
             f'role="img" aria-labelledby="t d">', STYLE,
             f'<rect class="bg" width="{w}" height="{h}" rx="8"/>',
             '<title id="t">Sustained throughput by configuration</title>',
             '<desc id="d">Median million events per second over repeated runs, with min-max range.</desc>',
             f'<text class="title" x="24" y="28">Sustained throughput by configuration</text>',
             f'<text class="subtitle" x="24" y="46">Saturating producer, 64-byte events, median of runs '
             f'(whisker: min-max). Higher is better.</text>']
    tick = 0
    while tick <= x_max:
        x = px(tick)
        parts.append(f'<line class="grid" x1="{x:.1f}" x2="{x:.1f}" y1="{top - 6}" y2="{h - 34}"/>')
        parts.append(f'<text class="tick" x="{x:.1f}" y="{h - 18}" text-anchor="middle">{tick}M/s</text>')
        tick += step
    for i, (label, med, lo, hi) in enumerate(data):
        y = top + i * (bar_h + gap)
        wbar = max(px(med) - left, 1)
        # Bar anchored at the baseline: square left end, 4px rounded data end.
        parts.append(f'<path style="fill:var(--s1)" d="M{left},{y} h{wbar - 4:.1f} a4,4 0 0 1 4,4 '
                     f'v{bar_h - 8} a4,4 0 0 1 -4,4 h{-(wbar - 4):.1f} z">'
                     f'<title>{esc(label.strip())}: {med:.2f}M/s (min {lo:.2f}, max {hi:.2f})</title></path>')
        cy = y + bar_h / 2
        parts.append(f'<line class="whisker" x1="{px(lo):.1f}" x2="{px(hi):.1f}" y1="{cy}" y2="{cy}"/>')
        parts.append(f'<text class="label" x="{left - 10}" y="{cy + 4}" text-anchor="end" '
                     f'xml:space="preserve">{esc(label)}</text>')
        parts.append(f'<text class="value" x="{px(max(hi, med)) + 8:.1f}" y="{cy + 4}">{med:.1f}M</text>')
    parts.append(f'<line class="axis" x1="{left}" x2="{left}" y1="{top - 6}" y2="{h - 34}"/>')
    parts.append("</svg>")
    out.write_text("\n".join(parts), encoding="utf-8")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("ablations", type=Path, help="docs/results/raw/<timestamp>_ablations")
    ap.add_argument("--out", type=Path, default=Path("docs/results/plots"))
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    latency_chart(args.ablations, args.out / "latency_percentiles.svg")
    throughput_chart(args.ablations, args.out / "throughput.svg")
    print(f"wrote {args.out}/latency_percentiles.svg and {args.out}/throughput.svg")


if __name__ == "__main__":
    main()
