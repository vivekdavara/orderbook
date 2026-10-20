# Changelog

## Unreleased

- `orderbook_latency --trace FILE`: each paced and saturated command of the engine's thread, `row,resting,index,
  due_ns,latency_ns`, and a test that every paced command is timed from its schedule (the day-5 review found that
  mechanism untested).

## v1.0.0 (2026-10-20)

The first release, built over five days (October 16 to 20, 2026). What a later 1.x keeps: the rules in DESIGN.md
(validation and the order of its checks, matching, the events and their order), the text format of commands, events
and scenario files, the gateway's protocol, the `orderbook` tool's commands and exit codes (0 ok, 1 a failed check or
bad input, 2 a usage error), and `Book`'s interface (`apply`, `depth`, `orders`, `best_price`, `size`,
`last_accepted_id`).

### The engine

- `Book` (`include/orderbook/book.hpp`): one instrument, price-time priority, limit, market, immediate-or-cancel and
  cancel orders, an event for each acceptance, rejection, trade and cancellation. Order nodes in a pool linked by
  32-bit index, an intrusive FIFO queue per price level, each side a sorted vector of levels with the best at the
  back, an open-addressing id table with backward-shift deletion. Nothing is allocated once the book has room
  (`BookCapacity`); past it, a failed allocation leaves the book well-formed. Copyable; moves leave the source empty
  and usable. (Day 2 built it on standard containers; day 3 rebuilt it with every event unchanged; day 2's is kept
  as `MapBook`, the baseline.)
- `EngineThread`: the book on its own spinning thread, between two lock-free single-producer single-consumer rings
  (`SpscRing`) of 16,384 requests and reports, with back-pressure both ways and a stop that applies everything sent.

### The gateway and market data

- `orderbook_gateway`: a TCP order gateway on 127.0.0.1:8107 in front of the engine's thread: one thread with
  `poll(2)`, a text protocol (`new`, `cancel`, `book`, `subscribe`), clients' own ids mapped to the engine's, a
  `fill` to each side of a trade, answers in line order, cancel on disconnect, limits on line length, unhandled input
  and unsent output.
- `DepthFeed`, `L2Snapshot`, `L2Book`: L2 market data built from the engine's events alone: snapshots, an update line
  for every command, subscribers that refuse gaps and repeats.

### Checking it

- DESIGN.md, the specification, and `ReferenceBook`, the rules as directly as code allows: the oracle.
- `Audit` and `check_book`, invariant checks that share none of the matching code; `RandomFlow`, a seeded command
  stream that is the same on libstdc++ and libc++; 15 hand-written scenario files; `minimize` (ddmin).
- The differential harness: the engine against the reference model, every event compared, any difference shrunk to a
  scenario file. 720,000 random commands in `make check`, 1.04 billion in `make soak` (a CI job), and both books in
  lockstep under libFuzzer.
- ASan and UBSan, ThreadSanitizer, an allocation-counting test binary, coverage, GCC 14, Clang 18 and Apple Clang
  with warnings as errors, and `make gcc-check` (GCC 15 here) before a push. Seven correctness reviews by a second
  agent (two on each of days 1 and 2, one on each day after), each finding reproduced and kept as a test.

### Measuring it

- `orderbook_bench` (Google Benchmark): the three books on the same commands at six book sizes, the ring, the engine
  with the depth feed.
- `orderbook_latency`: each command timed alone, and the engine's thread one at a time, paced (timed from when each
  command was due) and saturated.
- `orderbook_loadgen`: the gateway over TCP, each command timed from when it was due to its acknowledgement.
- Day 5: latency histograms. Both tools take `--csv FILE` (log-linear buckets, at most 3.1% wide from 32 ns up, in
  `bench/latency_histogram.hpp`), `scripts/plot_latency.py` draws them, and `make histograms` does both; the figures
  and the runs behind them are in `docs/`. The load generator's `--trace FILE` (every command's due time and latency,
  read by `scripts/trace_tail.py`) and `--lead-in N` (untimed commands before the timed ones) showed its paced tail
  to be a cold start.
- `scripts/release_check.sh`: every check, on the working tree and on a clean `git archive` of HEAD, with the version
  against `project()` and the release tag.

### The tool

- `orderbook`: `check`, `run`, `gen`, `audit`, `diff`, `replay`, and (day 5) `version`.

Known limitations are in README.md ("Limitations").
