"""Review 6 (17848a7): scripts/plot_latency.py on CSV files in the tools' own format (bench/latency_histogram.hpp's
buckets), each case failing on 17848a7 for the reason in its docstring.

    python3 tests/review6/plot_latency_review_test.py SCRIPT WORK_DIR CASE

CASE is one of the functions below; the exit code is 0 if it passes, 1 if it fails.
"""

import math
import os
import random
import re
import subprocess
import sys
import xml.etree.ElementTree as ElementTree

SVG = "{http://www.w3.org/2000/svg}"


# bench/latency_histogram.hpp's buckets, as the tools write them.
def bucket_of(v):
    if v < 64:
        return v
    e = v.bit_length() - 1 - 5
    return e * 32 + (v >> e)


def lower_of(b):
    if b < 64:
        return b
    return (b % 32 + 32) << (b // 32 - 1)


def upper_of(b):
    if b < 64:
        return b + 1
    return lower_of(b) + (1 << (b // 32 - 1))


def csv_lines(section, row, size, samples):
    counts = {}
    for s in samples:
        b = bucket_of(max(s, 0))
        counts[b] = counts.get(b, 0) + 1
    return [f"{section},{row},{size},{lower_of(b)},{upper_of(b)},{c}" for b, c in sorted(counts.items())]


ONE_THREAD = ["reference model", "day 2's engine (MapBook)", "engine"]
ENGINE_THREAD = ["one at a time", "paced 1M/s", "paced 5M/s", "saturated"]
GATEWAY = [("one at a time", 1), ("paced 50k/s", 4), ("paced 100k/s", 4), ("paced 200k/s", 4), ("saturated", 4)]


def write_csvs(work, latency, gateway):
    """latency(section, row, size) and gateway(row) give each row's samples; every row the script picks is there."""
    os.makedirs(work, exist_ok=True)
    lines = ["section,row,size,lower_ns,upper_ns,count"]
    for size in (70, 583, 5410):
        for name in ONE_THREAD:
            lines += csv_lines("one thread", name, size, latency("one thread", name, size))
        for name in ENGINE_THREAD:
            lines += csv_lines("engine thread", name, size, latency("engine thread", name, size))
    with open(os.path.join(work, "latency.csv"), "w") as f:
        f.write("\n".join(lines) + "\n")
    lines = ["section,row,size,lower_ns,upper_ns,count"]
    for name, size in GATEWAY:
        lines += csv_lines("gateway", name, size, gateway(name))
    with open(os.path.join(work, "loadgen.csv"), "w") as f:
        f.write("\n".join(lines) + "\n")


def plot(script, work):
    result = subprocess.run([sys.executable, script, os.path.join(work, "latency.csv"),
                             os.path.join(work, "loadgen.csv"), os.path.join(work, "figures")],
                            capture_output=True, text=True)
    return result


def lognormal(rnd, mu, sigma, n):
    return [int(rnd.lognormvariate(mu, sigma)) for _ in range(n)]


def a_brief_run_draws(script, work):
    """`orderbook_latency 2 --csv` and `orderbook_loadgen --commands 2 --csv` give rows of two samples each. The
    script divides by log10(the largest row's count) - log10(2), which is 0 for them: ZeroDivisionError at
    plot_latency.py:203 (x_end == x_start), no figures. A row of one sample fails too (the y range collapses)."""
    write_csvs(work, lambda s, n, z: [100 + 7 * len(n), 300 + 11 * len(n)], lambda n: [20_000, 30_000 + 100 * len(n)])
    result = plot(script, work)
    if result.returncode != 0:
        print("the script failed on rows of two samples:", result.stderr.strip().splitlines()[-1])
        return False
    return True


def a_row_of_one_sample_is_drawn_inside_the_plot(script, work):
    """A row with one sample, among rows of 1,000: its curve is drawn at x = 7.5, left of the y axis (LEFT = 70), as a
    path of zero length, so it is invisible in the plot area though its label says it is there."""
    rnd = random.Random(1)

    def latency(section, name, size):
        if section == "engine thread" and name == "paced 1M/s" and size == 583:
            return [100]
        return lognormal(rnd, 5, 1, 1000)

    write_csvs(work, latency, lambda n: lognormal(rnd, 10, 0.5, 1000))
    result = plot(script, work)
    if result.returncode != 0:
        print("the script failed:", result.stderr.strip().splitlines()[-1])
        return False
    root = ElementTree.parse(os.path.join(work, "figures", "engine_thread_percentiles.svg")).getroot()
    ok = True
    for path in root.iter(SVG + "path"):
        xs = [float(x) for x in re.findall(r"[ML](-?[0-9.]+),", path.get("d"))]
        if min(xs) < 69:  # (every curve starts a fraction of a pixel left of the axis, at rank ceil(n/2) - 1)
            print(f"a curve ({path.get('class')}) is drawn at x = {min(xs)}, left of the plot area (x >= 70)")
            ok = False
    return ok


def a_flat_density_is_drawn_flat_in_the_histogram(script, work):
    """The histogram's axis is "latency (log scale), bins an eighth of a doubling wide", but each bin is an eighth of
    its doubling's *linear* span: on the log axis the first bin of a doubling is log2(9/8) = 0.170 of a doubling wide
    and the last log2(16/15) = 0.093. Its heights are shares of commands per bin, so a sample spread evenly in
    log(latency) (a flat density on that axis) is drawn as a sawtooth falling 1.8x across each doubling and jumping
    back at every power of two (32.8 us, 65.5 us: the gateway's range), a step that isn't in the data. (Equal bins on
    the log axis, or heights divided by each bin's log width, would draw it flat.)"""
    rnd = random.Random(3)
    write_csvs(work, lambda s, n, z: lognormal(rnd, 5, 1, 1000),
               lambda n: [int(2 ** rnd.uniform(14, 16)) for _ in range(100_000)])
    result = plot(script, work)
    if result.returncode != 0:
        print("the script failed:", result.stderr.strip().splitlines()[-1])
        return False
    root = ElementTree.parse(os.path.join(work, "figures", "gateway_histogram.svg")).getroot()
    path = next(p for p in root.iter(SVG + "path") if "s1" in p.get("class").split())
    points = [(float(x), float(y)) for x, y in re.findall(r"[ML](-?[0-9.]+),(-?[0-9.]+)", path.get("d"))]
    baseline = points[0][1]
    # Each bin is a horizontal segment above the baseline; keep the ones well inside [2^14, 2^16) (not the edge bins).
    heights = [baseline - a[1] for a, b in zip(points, points[1:]) if a[1] == b[1] and a[1] != baseline and b[0] > a[0]]
    inner = heights[1:-1]
    ratio = max(inner) / min(inner)
    print(f"{len(heights)} bins; a flat density drawn {min(inner):.1f} to {max(inner):.1f} px high ({ratio:.2f}x)")
    # On a log share axis (since 8b353a4) a 1.85x sawtooth is a spread of about 13 px in the bins' tops, not a ratio
    # of heights; the bins' sampling noise here (about 6,000 commands each) is well under a pixel. (Added when the
    # review's test was brought in: the ratio alone passed the unnormalised bins on the log axis, at 1.07x.)
    spread = max(inner) - min(inner)
    print(f"the bins' tops are {spread:.1f} px apart")
    return ratio < 1.15 and spread < 3


def the_end_labels_fit_in_the_figure(script, work):
    """Each curve's label goes right of the plot, at x = 638 of an 820-px figure (182 px). "day 2's engine (MapBook),
    max 2.05 us" is 37 characters; in Chrome these 12-px labels measure 6.0 to 6.1 px a character, and that one ends
    at x = 861, 41 px past the figure's edge, cut off. (The script's own legend allows 7.2 px a character.)"""
    rnd = random.Random(4)
    write_csvs(work, lambda s, n, z: lognormal(rnd, 5, 1, 1000), lambda n: lognormal(rnd, 10, 0.5, 1000))
    result = plot(script, work)
    if result.returncode != 0:
        print("the script failed:", result.stderr.strip().splitlines()[-1])
        return False
    ok = True
    for name in sorted(os.listdir(os.path.join(work, "figures"))):
        root = ElementTree.parse(os.path.join(work, "figures", name)).getroot()
        width = float(root.get("width"))
        for text in root.iter(SVG + "text"):
            if text.get("text-anchor") != "start" or text.get("class") != "label":
                continue
            end = float(text.get("x")) + 6.0 * len(text.text)
            if end > width:
                print(f"{name}: '{text.text}' ends at about x = {end:.0f}, past the figure's {width:.0f} px")
                ok = False
    return ok


CASES = {f.__name__: f for f in (a_brief_run_draws, a_row_of_one_sample_is_drawn_inside_the_plot,
                                 a_flat_density_is_drawn_flat_in_the_histogram, the_end_labels_fit_in_the_figure)}

if __name__ == "__main__":
    script, work, case = sys.argv[1:4]
    sys.exit(0 if CASES[case](script, os.path.join(work, case)) else 1)
