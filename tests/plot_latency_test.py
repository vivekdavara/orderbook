"""Which rows scripts/plot_latency.py's figures take: the engine thread's figure the middle of orderbook_latency's
book sizes, the one-thread figure the largest, each named in its subtitle (`plot_latency.each_figure_takes_...`).

    python3 tests/plot_latency_test.py SCRIPT REVIEW6_TEST WORK_DIR

It writes CSV files with tests/review6/plot_latency_review_test.py's helpers (the tools' buckets, sizes 70, 583 and
5,410), gives each size's rows a different latency, and checks each figure's subtitle and where its curves sit.
"""

import importlib.util
import os
import re
import sys
import xml.etree.ElementTree as ElementTree

script, review6, work = sys.argv[1:4]
spec = importlib.util.spec_from_file_location("review6", review6)
helpers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helpers)

# Each size's rows take a latency of their own (ns), each a bucket's lower bound (a figure draws buckets at their lower
# bounds), so a figure's values say which size it drew.
latency_of_size = {70: 1_024, 583: 20_480, 5410: 409_600}
assert all(helpers.lower_of(helpers.bucket_of(v)) == v for v in latency_of_size.values())
helpers.write_csvs(work, lambda section, name, size: [latency_of_size[size]] * 100, lambda name: [30_000] * 100)
result = helpers.plot(script, work)
if result.returncode != 0:
    sys.exit(f"the script failed: {result.stderr}")

ok = True
for figure, size in (("engine_thread_percentiles.svg", 583), ("one_thread_percentiles.svg", 5410)):
    root = ElementTree.parse(os.path.join(work, "figures", figure)).getroot()
    texts = [t.text for t in root.iter(helpers.SVG + "text")]
    if not any(f"{size:,} resting orders" in t for t in texts):
        print(f"{figure}: no subtitle names {size:,} resting orders: {texts[:3]}")
        ok = False
    # The end labels give each curve's slowest command, which is this size's latency.
    expected = {583: "20.5 µs", 5410: "410 µs"}[size]
    labels = [t for t in texts if ", max " in t]
    if not labels or not all(t.endswith(expected) for t in labels):
        print(f"{figure}: expected every curve to end at {expected}: {labels}")
        ok = False
sys.exit(0 if ok else 1)
