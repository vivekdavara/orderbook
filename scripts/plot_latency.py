#!/usr/bin/env python3
"""Draws the latency figures from the measuring tools' --csv files, as SVG (`make histograms`).

    scripts/plot_latency.py LATENCY_CSV LOADGEN_CSV OUT_DIR [--note TEXT]

LATENCY_CSV is `orderbook_latency --csv`'s file, LOADGEN_CSV `orderbook_loadgen --csv`'s; each line is one histogram
bucket of one row (bench/latency_histogram.hpp). It writes four figures:

  engine_thread_percentiles.svg  the engine's thread from the sender's side, on the middle of the tool's book sizes
  one_thread_percentiles.svg     each book's own time a command, on the largest
  gateway_percentiles.svg        the gateway over TCP, from when each command was due to its acknowledgement
  gateway_histogram.svg          the same rows as shares of commands by latency, to show where the tail's commands sit

A percentile curve is drawn from the buckets, each at its lower bound, so a point is at most 3.1% below the exact
nearest-rank value the tool printed for it (from 32 ns up; below 64 ns the buckets are exact). A row of any size, one
sample included, is drawn. The standard library only: no plotting package to install.
"""

import argparse
import csv
import math
import os
import sys
import xml.etree.ElementTree as ElementTree
from collections import defaultdict
from xml.sax.saxutils import escape

# The dataviz reference palette's first four categorical slots, in its fixed order, light and dark steps.
SERIES_LIGHT = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"]
SERIES_DARK = ["#3987e5", "#d95926", "#199e70", "#c98500"]

WIDTH, HEIGHT = 840, 460
LEFT, RIGHT, TOP, BOTTOM = 70, 200, 92, 84

# Shorter names for the end-of-line labels, where the right margin is narrow.
SHORT = {"day 2's engine (MapBook)": "MapBook (day 2)"}


def read_rows(path):
    """{(section, row, size): [(lower, upper, count), ...] lowest first}."""
    rows = defaultdict(list)
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        expected = ["section", "row", "size", "lower_ns", "upper_ns", "count"]
        if reader.fieldnames != expected:
            sys.exit(f"plot_latency: {path}: columns {reader.fieldnames}, expected {expected}")
        for line in reader:
            key = (line["section"], line["row"], int(line["size"]))
            rows[key].append((int(line["lower_ns"]), int(line["upper_ns"]), int(line["count"])))
    for buckets in rows.values():
        buckets.sort()
    return rows


def pick(rows, section, size, names, path):
    picked = []
    for name in names:
        key = (section, name, size)
        if key not in rows:
            sys.exit(f"plot_latency: {path} has no row {section!r} / {name!r} / {size}")
        picked.append((name, rows[key]))
    return picked


def duration(ns):
    """A latency in ns, µs or ms, to three significant figures."""
    for unit, scale in (("ms", 1e6), ("µs", 1e3)):
        if ns >= scale:
            return f"{ns / scale:.3g} {unit}"
    return f"{ns:.3g} ns"


def log_ticks(lo, hi):
    """Tick values between lo and hi: 1-2-5 steps over three decades or fewer, powers of ten past that."""
    decades = math.log10(hi / lo)
    steps = (1, 2, 5) if decades <= 3 else (1,)
    ticks = []
    for exponent in range(math.floor(math.log10(lo)), math.ceil(math.log10(hi)) + 1):
        for step in steps:
            value = step * 10.0 ** exponent
            if lo <= value * (1 + 1e-9) and value <= hi * (1 + 1e-9):
                ticks.append(value)
    return ticks


def nice_range(lo, hi):
    """The 1-2-5 values just outside [lo, hi]."""
    def below(v):
        e = math.floor(math.log10(v))
        return max(s * 10.0 ** e for s in (1, 2, 5) if s * 10.0 ** e <= v * (1 + 1e-9))

    def above(v):
        e = math.floor(math.log10(v))
        return min(s * 10.0 ** e for s in (1, 2, 5, 10) if s * 10.0 ** e >= v * (1 - 1e-9))

    lo, hi = below(max(lo, 1)), above(max(hi, 1))
    return (lo, hi) if hi > lo else (lo, above(lo * 1.01))  # never an empty range: one value still gets an axis


class Svg:
    def __init__(self, title, subtitle, note):
        self.parts = []
        self.title, self.subtitle, self.note = title, subtitle, note

    def add(self, text):
        self.parts.append(text)

    def text(self, x, y, s, cls, anchor="start"):
        self.add(f'<text x="{x:.1f}" y="{y:.1f}" class="{cls}" text-anchor="{anchor}">{escape(s)}</text>')

    def write(self, path):
        light = "".join(f"--s{i + 1}: {c}; " for i, c in enumerate(SERIES_LIGHT))
        dark = "".join(f"--s{i + 1}: {c}; " for i, c in enumerate(SERIES_DARK))
        style = (
            f"svg {{ --surface: #fcfcfb; --ink: #0b0b0b; --ink2: #52514e; --muted: #898781; --grid: #e1e0d9; "
            f"--axis: #c3c2b7; {light}}}\n"
            f"@media (prefers-color-scheme: dark) {{ svg {{ --surface: #1a1a19; --ink: #f0efec; --ink2: #c3c2b7; "
            f"--grid: #2c2c2a; --axis: #383835; {dark}}} }}\n"
            "text { font-family: system-ui, -apple-system, 'Segoe UI', sans-serif; }\n"
            ".surface { fill: var(--surface); }\n"
            ".title { fill: var(--ink); font-size: 15px; font-weight: 600; }\n"
            ".subtitle, .label { fill: var(--ink2); font-size: 12px; }\n"
            ".tick { fill: var(--muted); font-size: 11px; font-variant-numeric: tabular-nums; }\n"
            ".note { fill: var(--muted); font-size: 10.5px; }\n"
            ".grid { stroke: var(--grid); stroke-width: 1; }\n"
            ".axis { stroke: var(--axis); stroke-width: 1; }\n"
            + "".join(f".s{i + 1} {{ stroke: var(--s{i + 1}); }}\n" for i in range(4))
            + ".line { fill: none; stroke-width: 2; stroke-linejoin: round; stroke-linecap: round; }\n"
        )
        head = [
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" height="{HEIGHT}" '
            f'viewBox="0 0 {WIDTH} {HEIGHT}" role="img" aria-label="{escape(self.title)}">',
            f"<title>{escape(self.title)}</title>",
            f"<style>\n{style}</style>",
            f'<rect class="surface" width="{WIDTH}" height="{HEIGHT}" rx="8"/>',
        ]
        self.parts = head + self.parts
        self.text(LEFT, 26, self.title, "title")
        self.text(LEFT, 44, self.subtitle, "subtitle")
        if self.note:
            self.text(LEFT, HEIGHT - 12, self.note, "note")
        self.add("</svg>")
        with open(path, "w") as f:
            f.write("\n".join(self.parts) + "\n")
        ElementTree.parse(path)  # well-formed, or this raises
        print(f"wrote {path}")


def legend(svg, names):
    x = LEFT
    for i, name in enumerate(names):
        svg.add(f'<line class="line s{i + 1}" x1="{x}" y1="64" x2="{x + 18}" y2="64"/>')
        svg.text(x + 24, 68, name, "label")
        x += 24 + 7.2 * len(name) + 22


def spread(ys, gap=14.0, lo=TOP, hi=HEIGHT - BOTTOM):
    """Moves label positions apart, keeping their order, so none is closer than `gap` to the next."""
    order = sorted(range(len(ys)), key=lambda i: ys[i])
    placed = [ys[i] for i in order]
    for k in range(1, len(placed)):
        placed[k] = max(placed[k], placed[k - 1] + gap)
    overflow = placed[-1] - hi if placed else 0
    if overflow > 0:
        placed = [max(lo, y - overflow) for y in placed]
    result = [0.0] * len(ys)
    for k, i in enumerate(order):
        result[i] = placed[k]
    return result


def percentile_figure(series, path, title, subtitle, note):
    """Latency by percentile: x = log10(1 / (1 - p)) from the median to the last sample, y = log latency."""
    curves = []
    for name, buckets in series:
        total = sum(c for _, _, c in buckets)
        start_rank = math.ceil(0.5 * total)
        points = []  # (p, value), p the share of samples at or below
        seen = 0
        for lower, _, count in buckets:
            before, seen = seen, seen + count
            if seen < start_rank:
                continue
            points.append((max(before, start_rank - 1) / total, max(lower, 1)))
        curves.append((name, total, points))
    x_start = math.log10(2)
    # The last sample of the largest row, and at least a decade past the median, so that rows of one or two samples
    # still have an axis.
    x_end = max(max(math.log10(total) for _, total, _ in curves), x_start + 1)
    values = [v for _, _, pts in curves for _, v in pts]
    y_lo, y_hi = nice_range(min(values), max(values))
    plot_w, plot_h = WIDTH - LEFT - RIGHT, HEIGHT - TOP - BOTTOM

    def px(p, total):
        x = min(math.log10(1 / (1 - p)) if p < 1 else x_end, math.log10(total))
        x = max(x, x_start)  # a row of one sample is its median and its slowest: drawn at the median, not left of it
        return LEFT + (x - x_start) / (x_end - x_start) * plot_w

    def py(v):
        return TOP + plot_h - (math.log10(v) - math.log10(y_lo)) / (math.log10(y_hi) - math.log10(y_lo)) * plot_h

    svg = Svg(title, subtitle, note)
    for v in log_ticks(y_lo, y_hi):
        y = py(v)
        svg.add(f'<line class="grid" x1="{LEFT}" y1="{y:.1f}" x2="{LEFT + plot_w}" y2="{y:.1f}"/>')
        svg.text(LEFT - 8, y + 4, duration(v), "tick", "end")
    for p, label in ((0.5, "50%"), (0.9, "90%"), (0.99, "99%"), (0.999, "99.9%"), (0.9999, "99.99%"),
                     (0.99999, "99.999%"), (0.999999, "99.9999%")):
        if math.log10(1 / (1 - p)) > x_end + 1e-9:
            break
        x = LEFT + (math.log10(1 / (1 - p)) - x_start) / (x_end - x_start) * plot_w
        svg.add(f'<line class="grid" x1="{x:.1f}" y1="{TOP}" x2="{x:.1f}" y2="{TOP + plot_h}"/>')
        svg.text(x, TOP + plot_h + 18, label, "tick", "middle")
    svg.add(f'<line class="axis" x1="{LEFT}" y1="{TOP + plot_h}" x2="{LEFT + plot_w}" y2="{TOP + plot_h}"/>')
    svg.text(LEFT + plot_w / 2, TOP + plot_h + 40, "percentile of commands (log scale; each line ends at its slowest)",
             "label", "middle")

    ends = []
    for i, (name, total, points) in enumerate(curves):
        path_points = []
        for k, (p, v) in enumerate(points):
            x = px(p, total)
            if k > 0:
                path_points.append((x, py(points[k - 1][1])))
            path_points.append((x, py(v)))
        x_last = px(1, total)
        path_points.append((x_last, py(points[-1][1])))
        d = " ".join(f"{'M' if k == 0 else 'L'}{x:.1f},{y:.1f}" for k, (x, y) in enumerate(path_points))
        svg.add(f'<path class="line s{i + 1}" d="{d}"/>')
        ends.append((x_last, py(points[-1][1]), name, points[-1][1]))
    label_ys = spread([y for _, y, _, _ in ends])
    for (x, _, name, v), y in zip(ends, label_ys):
        svg.text(LEFT + plot_w + 8, y + 4, f"{SHORT.get(name, name)}, max {duration(v)}", "label")
    legend(svg, [name for name, _, _ in curves])
    svg.write(path)


def histogram_figure(series, path, title, subtitle, note):
    """Each row's commands by latency, on log axes. The bins are 4 of the CSV's buckets (an eighth of a doubling's
    span, so on the log axis from 0.093 to 0.170 of a doubling wide), and each bin's share is divided by its width on
    that axis, in eighths of a doubling: a share per eighth of a doubling, which draws a distribution that is flat in
    log(latency) flat. The share axis is logarithmic too, so that a tail of a few commands a bin shows beside a mode of
    a fifth of them."""
    curves = []
    for name, buckets in series:
        total = sum(c for _, _, c in buckets)
        bins = defaultdict(int)
        for lower, upper, count in buckets:
            if lower < 64:  # exact buckets: values below 64 ns, a bin each (0 drawn from 1 ns, on a log axis)
                bins[(max(lower, 1), max(upper, 2))] += count
                continue
            e = (lower.bit_length() - 1) - 5  # the doubling, as in bucket_of
            width = 1 << e
            first = (lower // (4 * width)) * (4 * width)
            bins[(first, first + 4 * width)] += count
        density = sorted((lo, hi, c / total / (8 * math.log2(hi / lo))) for (lo, hi), c in bins.items())
        curves.append((name, total, density))
    x_lo, x_hi = nice_range(min(b[0][0] for _, _, b in curves), max(b[-1][1] for _, _, b in curves))
    # From one command of the largest row in the widest bin, rounded down to a power of ten, to the largest density.
    y_lo = 10.0 ** math.floor(math.log10(1 / max(total for _, total, _ in curves) / (8 * math.log2(9 / 8))))
    y_hi = 10.0 ** math.ceil(math.log10(max(d for _, _, b in curves for _, _, d in b)))
    if y_hi <= y_lo:
        y_lo = y_hi / 10
    plot_w, plot_h = WIDTH - LEFT - RIGHT, HEIGHT - TOP - BOTTOM

    def px(v):
        return LEFT + (math.log10(v) - math.log10(x_lo)) / (math.log10(x_hi) - math.log10(x_lo)) * plot_w

    def py(share):
        share = max(share, y_lo)
        return TOP + plot_h - (math.log10(share) - math.log10(y_lo)) / (math.log10(y_hi) - math.log10(y_lo)) * plot_h

    svg = Svg(title, subtitle, note)
    exponent = round(math.log10(y_lo))
    while 10.0 ** exponent <= y_hi * (1 + 1e-9):
        y = py(10.0 ** exponent)
        svg.add(f'<line class="grid" x1="{LEFT}" y1="{y:.1f}" x2="{LEFT + plot_w}" y2="{y:.1f}"/>')
        svg.text(LEFT - 8, y + 4, f"{100 * 10.0 ** exponent:g}%", "tick", "end")
        exponent += 1
    for v in log_ticks(x_lo, x_hi):
        x = px(v)
        svg.add(f'<line class="grid" x1="{x:.1f}" y1="{TOP}" x2="{x:.1f}" y2="{TOP + plot_h}"/>')
        svg.text(x, TOP + plot_h + 18, duration(v), "tick", "middle")
    svg.add(f'<line class="axis" x1="{LEFT}" y1="{TOP + plot_h}" x2="{LEFT + plot_w}" y2="{TOP + plot_h}"/>')
    svg.text(LEFT + plot_w / 2, TOP + plot_h + 40,
             "latency (log scale); share of the row's commands per eighth of a doubling (log scale)",
             "label", "middle")
    for i, (name, _, bins) in enumerate(curves):
        points = [(px(bins[0][0]), py(0))]
        previous_hi = None
        for lo, hi, share in bins:
            if previous_hi is not None and lo > previous_hi:  # empty bins between: down to the floor
                points += [(px(previous_hi), py(0)), (px(lo), py(0))]
            points += [(px(lo), py(share)), (px(hi), py(share))]
            previous_hi = hi
        points.append((px(previous_hi), py(0)))
        d = " ".join(f"{'M' if k == 0 else 'L'}{x:.1f},{y:.1f}" for k, (x, y) in enumerate(points))
        svg.add(f'<path class="line s{i + 1}" d="{d}"/>')
    legend(svg, [name for name, _, _ in curves])
    svg.write(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("latency_csv")
    parser.add_argument("loadgen_csv")
    parser.add_argument("out_dir")
    parser.add_argument("--note", default="", help="a line of conditions at the foot of each figure")
    args = parser.parse_args()
    latency = read_rows(args.latency_csv)
    loadgen = read_rows(args.loadgen_csv)
    os.makedirs(args.out_dir, exist_ok=True)
    out = lambda name: os.path.join(args.out_dir, name)  # noqa: E731

    # orderbook_latency's books, by the orders resting after each warm-up: the middle one for the engine's thread,
    # the largest for the books themselves, where the reference model's scan costs most.
    sizes = sorted({size for section, _, size in latency if section == "engine thread"})
    middle = sizes[len(sizes) // 2]
    percentile_figure(
        pick(latency, "engine thread", middle, ["one at a time", "paced 1M/s", "paced 5M/s", "saturated"],
             args.latency_csv),
        out("engine_thread_percentiles.svg"),
        "The engine's thread, from sending a command to receiving its last event",
        f"orderbook_latency, {middle:,} resting orders; paced commands timed from when they were due",
        args.note)
    largest = max(size for section, _, size in latency if section == "one thread")
    percentile_figure(
        pick(latency, "one thread", largest, ["reference model", "day 2's engine (MapBook)", "engine"],
             args.latency_csv),
        out("one_thread_percentiles.svg"),
        "Each book's own time a command, one thread",
        f"orderbook_latency, {largest:,} resting orders; one clock read either side (it steps in about 42 ns)",
        args.note)
    gateway = []
    for name, connections in (("one at a time", 1), ("paced 50k/s", 4), ("paced 100k/s", 4), ("paced 200k/s", 4)):
        gateway += pick(loadgen, "gateway", connections, [name], args.loadgen_csv)
    percentile_figure(
        gateway, out("gateway_percentiles.svg"),
        "The gateway over TCP, from when a command was due to its acknowledgement",
        "orderbook_loadgen; one at a time on 1 connection, paced over 4",
        args.note)
    histogram_figure(
        gateway, out("gateway_histogram.svg"),
        "Where the gateway's commands sit: share of commands by latency",
        "orderbook_loadgen, the rows of the percentile figure; one at a time on 1 connection, paced over 4",
        args.note)


if __name__ == "__main__":
    main()
