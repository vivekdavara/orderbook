#!/usr/bin/env python3
"""When were the slow commands due? Reads `orderbook_loadgen --trace FILE` and prints, for each row:

  - its episodes of slow commands (slower than --slow, 1 ms by default): runs of them whose due times are less than
    5 ms apart, with when they were due, how many, and the worst;
  - nearest-rank percentiles of all its commands, and again without those due in its first --skip milliseconds
    (50 and 100 by default), to see how much of the tail is the start of the row.

    scripts/trace_tail.py TRACE [--slow US] [--skip MS]...

The standard library only.
"""

import argparse
import csv
import math
import sys
from collections import defaultdict


def nearest_rank(ordered, q):
    rank = max(1, math.ceil(q * len(ordered) - 1e-9))
    return ordered[min(len(ordered), rank) - 1]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("trace")
    parser.add_argument("--slow", type=float, default=1000.0, help="slower than this many microseconds is slow")
    parser.add_argument("--skip", type=float, action="append", help="milliseconds to leave out at a row's start")
    args = parser.parse_args()
    skips = [0.0] + (args.skip or [50.0, 100.0])

    rows = defaultdict(list)  # row -> [(due_us, latency_us)], in the order of the file
    with open(args.trace, newline="") as f:
        reader = csv.DictReader(f)
        if reader.fieldnames != ["row", "index", "connection", "due_us", "latency_us"]:
            sys.exit(f"trace_tail: {args.trace} isn't a load generator trace (columns {reader.fieldnames})")
        for line in reader:
            rows[line["row"]].append((float(line["due_us"]), float(line["latency_us"])))

    for name, commands in rows.items():
        span_ms = max(due for due, _ in commands) / 1e3
        slow = sorted((due, latency) for due, latency in commands if latency > args.slow)
        print(f"{name}: {len(commands)} commands due over {span_ms:.0f} ms, {len(slow)} slower than "
              f"{args.slow:g} us")
        episodes = []  # [first due, last due, count, worst]
        for due, latency in slow:
            if episodes and due - episodes[-1][1] < 5000:
                episode = episodes[-1]
                episode[1], episode[2], episode[3] = due, episode[2] + 1, max(episode[3], latency)
            else:
                episodes.append([due, due, 1, latency])
        for first, last, count, worst in episodes:
            print(f"  due {first / 1e3:8.2f} to {last / 1e3:8.2f} ms: {count:6d} slow, the worst {worst / 1e3:.2f} ms")
        print(f"  {'leaving out':>11} {'commands':>9} {'p50':>8} {'p90':>8} {'p99':>9} {'p99.9':>9} {'max':>9}   (us)")
        for skip in skips:
            kept = sorted(latency for due, latency in commands if due >= skip * 1e3)
            if not kept:
                continue
            print(f"  {f'first {skip:g} ms' if skip else 'nothing':>11} {len(kept):9d} "
                  f"{nearest_rank(kept, 0.50):8.1f} {nearest_rank(kept, 0.90):8.1f} {nearest_rank(kept, 0.99):9.1f} "
                  f"{nearest_rank(kept, 0.999):9.1f} {kept[-1]:9.1f}")


if __name__ == "__main__":
    main()
