# Walkthrough

Questions an interviewer is likely to ask about this project, with short answers grounded in the code. Day 1 covers
the specification, the reference model and the test kit; day 2 the engine and the differential tests; day 3 the fast
engine and its thread; day 4 the TCP gateway and the market data; day 5 adds the release.

Reading order for day 1: DESIGN.md ("Validation", "Matching", "Events", "Invariants"), then
`src/reference_book.cpp` (one screen), a scenario file such as `tests/scenarios/03_time_priority.txt`, then
`src/check.cpp` (`Audit::record_new`) and `src/random_flow.cpp`.

Reading order for day 2: `include/orderbook/map_book.hpp` and `src/map_book.cpp` (day 2's engine, kept as `MapBook`:
`new_order`, `rest`, `cancel`), then `include/orderbook/differential.hpp` (`Lockstep`, `first_difference`,
`shrink_to_scenario`), then `tests/differential_test.cpp` (`expect_same_books` and the seven mixes).

Reading order for day 3: DESIGN.md "The engine" and "Threads (day 3)", then `src/book.cpp` (`new_order`, `rest`,
`cancel`, `allocate`, `release`, the moves), `include/orderbook/id_table.hpp` (`find`, `take`), `include/orderbook/
spsc_ring.hpp` (`try_push`, `try_pop`), `src/engine_thread.cpp` (`run`), then `tests/allocation_test.cpp` and
`bench/latency.cpp` (`paced`).

Reading order for day 4: DESIGN.md "Market data (day 4)" and "The gateway (day 4)", then `src/market_data.cpp`
(`DepthFeed::apply`, `follow_new_order`, `take`; `L2Book::apply`), `include/orderbook/gateway.hpp` (the protocol),
`src/gateway.cpp` (`run`, `handle_lines`, `handle_line`, `handle_report`, `command_answered`, `flush_held`,
`end_sessions`), `tools/gateway_main.cpp` (the signals), then `tests/gateway_test.cpp` and `bench/loadgen.cpp`
(`drive`).

Reading order for day 5: README "Results at a glance" and "Results (v1.0.0)", then `bench/latency_histogram.hpp`
(`bucket_of`, `lower_of`, `at_quantile`, `CsvWriter`), `scripts/plot_latency.py` (`percentile_figure`), the trace in
`bench/loadgen.cpp` (`row`) and `scripts/trace_tail.py`, then `tests/latency_histogram_test.cpp`, CHANGELOG.md and
`docs/data/`.

## The project

**What is it, in a minute?**
A matching engine for one instrument's limit order book. Clients send new orders (limit, IOC, market) and cancels;
the engine answers each with events: accepted or rejected, trades, cancellations. Day 1 is the specification and
everything needed to check an implementation against it: a reference model, hand-written scenario files, a seeded
random command stream, and invariant checks. Day 2 writes the fast engine and compares it with the reference event
for event; day 3 makes it fast; day 4 puts it behind a TCP gateway.

**What is price-time priority, and where is it in the code?**
An incoming order trades first with the best price on the other side (the lowest ask for a buy, the highest bid for
a sell), and among orders at that price, with the one that arrived first. In the reference model it is the loop in
`ReferenceBook::new_order` (`src/reference_book.cpp`): it scans the other side and keeps the first order with a
strictly better price, so of two orders at the same price the earlier one wins. Each side is a vector in arrival
order, and erasing keeps that order, so "first in the vector" is "arrived first".

**Why is the trade at the maker's price, not the taker's?**
The resting order's price was public and the taker agreed to pay at least that much. A buy at 105 that finds an
ask at 100 trades at 100: the taker gets the better price (`tests/scenarios/04_trade_price.txt`). That is how
continuous double auctions on exchanges work, and it means a trade's price never depends on how aggressive the
taker was.

**What happens to the part of an order that doesn't trade?**
A limit order's remainder rests at its own price, behind the orders already there (`10_sweep_and_rest.txt`). An
IOC's or a market order's is cancelled at once with `cancelled <id> <remaining> unfilled` (`06_ioc.txt`,
`07_market.txt`); neither ever rests. An IOC that doesn't cross at all is accepted and then cancelled whole, not
rejected: it was a valid order that found nothing.

**Can the book be locked (best bid equal to best ask)?**
No. A buy at the best ask's price crosses it (rule 1 uses "at or below"), so it trades instead of resting; the same
for a sell at the best bid (`10_sweep_and_rest.txt`, "equal prices trade"). After every command, best bid < best
ask is invariant I1, and `check_book` checks it.

## Design choices

**Why must order ids increase?**
Three reasons. A duplicate is detected with one comparison (`order.id <= last_accepted_`), with no set of every id
ever seen, which would grow forever. An id is never reused, so a late cancel can't remove a newer order that
happened to get the same id. And the rule is trivial for the gateway to meet, because the gateway assigns engine ids
(day 4) and maps clients' own ids to them. The cost: a client can't choose engine ids, which it shouldn't anyway.

**Why does a rejection change nothing, not even the last accepted id?**
So the rule is easy to state and to test: `ARejectedCommandChangesNothing` compares the whole book before and after.
It also lets a client fix a rejected order and resend it under the same id (`09_validation.txt`).

**Why does the order of the validation checks matter?**
An order can break several rules at once (`new 5 buy limit 0 @ 0` after id 5 was accepted breaks all three). If
two implementations checked in different orders, they would reject it for different reasons, and a differential
test would report a difference that isn't a bug in either. So DESIGN.md fixes the order (id, quantity, price), and
`ChecksTheRulesInTheirSpecifiedOrder` tests it.

**Why integers for prices, and why wrap them in types?**
Floating-point prices can't be compared for equality reliably (0.1 + 0.2 != 0.3), and the book groups orders by
equal price. Integer ticks are exact. Wrapping `Price`, `Quantity` and `OrderId` in their own structs means
`NewOrder{id, side, type, price, quantity}` with the last two swapped doesn't compile; `types_test.cpp` asserts they
don't convert into each other and are still trivially copyable 8-byte values.

**Why one `trade` event naming both orders?**
It's the engine's internal event; one fill is one fact. The gateway turns it into an execution report for each
side. Two events per fill would double the engine's output for no information.

## The engine

The first questions here are about day 2's engine, which is now `MapBook` (`src/map_book.cpp`), kept as the
baseline; day 3's rebuild is under "Performance (day 3)".

**How was the engine built on day 2?**
`MapBook` (`src/map_book.cpp`; it was `Book` until day 3) keeps each side as a `std::map` from price to level, best
first, so the best price is `begin()`. A level is a `std::list` of `{id, remaining}` in arrival order plus its total
quantity. An `std::unordered_map` from id to `{side, map iterator, list iterator}` finds a resting order without
searching. A taker walks the opposite map from `begin()` while it crosses, trades with each level's queue from the
front, erases filled makers from the queue and the index, and erases a level when it empties; a limit remainder is
pushed to the back of its level (`try_emplace` makes the level if it's new) and indexed.

**What does each operation cost?**
Resting a new order is O(log L) for its level (L levels on that side) and O(1) to queue and index it. Each fill is
O(1), and erasing an emptied level by iterator is amortised O(1). A cancel is O(1) amortised: index lookup, list
unlink, and a level erase by iterator if it emptied. Best price is O(1), depth O(L), since each level keeps its total.

**Why one comparator type for both sides?**
`BetterPrice{side}` compares higher-first for bids and lower-first for asks. With `std::greater` and `std::less` the
two maps would be different types, so the index would need two iterator types (a variant, or two indexes) and the
matching code would become a template. One type keeps one `Location` struct and one code path; the cost is a
well-predicted branch per comparison, and day 3 replaces the map anyway.

**Why was day 2's `Book` move-only?** (Day 3's is copyable: see "Performance (day 3)".)
Its index holds iterators into its own map and lists. A defaulted copy would copy those iterators, so the copy's
cancels would unlink nodes in the original: a use-after-free once the original goes. Copying would need the index
rebuilt for the copy's nodes, and nothing needs a copy, so the copy constructor is deleted (`book_test.cpp` asserts
it). A move is safe: node-based containers hand their nodes to the new container, so every iterator still refers to
the same element, now in the moved-to book; a test moves a book and then cancels and trades in it, under ASan too.

**Why was day 2's engine slower than the reference model on small books?**
At 7 resting orders the reference model did 86.0M commands/s and the engine 20.4M; at 70, 25.3M and 20.9M; at 583
the engine is 5.2 times faster, at 5,410, 37 times (`make bench`, `b0e54b2`, on battery). A side of 7 orders is a few
cache lines of a vector the reference scans without allocating. The engine allocates a list node and a hash node for
each order that rests (and a map node for a new level), frees them as orders fill and cancel, and follows pointers
through all three. Day 3 is about exactly that: intrusive lists over a pool, an open-addressing index.

**Why doesn't the engine reuse the reference model's validation?**
Shared code can't disagree with itself, so a mistake in it would pass every differential test. The engine has its
own validation function, so the order of the checks is checked differentially (and by the Audit, independently of
both). But both call the same bounds predicates, `valid_price` and `valid_quantity` in `types.hpp`: the day-2 review
made the price bound exclusive there, and every differential run still passed. Only the hand-written tests caught it
(`Books.BoundsAreInclusive`, the validation scenario, `types_test.cpp`). Shared vocabulary needs its own tests.

**Is the engine deterministic, with a hash table inside?**
Iteration order of an `unordered_map` differs between standard libraries, but day 2's engine never iterated its index,
and day 3's never iterates its own `IdTable` either: they only look ids up. Everything that decides events or the
reported state walks the levels and their queues, whose order is the book's. A test requires the engine's events and
final book on 50,000 commands to hash to the reference model's pinned value, on every CI job (GCC, Clang, Apple Clang;
Debug, Release, sanitizers).

## Performance (day 3)

**What did day 3 change, and what stayed the same?**
The engine's insides, not one event. `Book` is now a pool of order nodes linked by 32-bit indices, each level an
intrusive FIFO of them, each side a vector of levels sorted worst first (best at the back), and an open-addressing
id table; day 2's engine stays as `MapBook` in the test kit, so both are measured in the same runs. Every test that
ran on day 2's engine ran on the new one and passed first time, unchanged but for the assertion that a `Book` can't be
copied: the scenario files, the rule suite, 720,000 differential commands in `make check`, 1.04 billion in `make soak`
(39 s), the pinned determinism hash.

**Why indices instead of pointers?**
Half the size (4 bytes against 8, so a node is 40 bytes), and the book becomes position-independent: the pool is a
`std::vector` that may reallocate when it grows, and a pointer into it would dangle; an index doesn't. It also makes
copying and moving the book member by member correct, which day 2's iterator-holding index couldn't be. The cost is
an add per hop (`nodes_[i]`), which is free next to a cache miss.

**Why a sorted vector of levels, with the best at the back?**
Most activity is at the top of the book. With the best level at the back, the best price is `back()`, a taker fills
from the back and pops levels it empties, and a new level near the top is inserted near the back, so few entries move.
The levels sit in one contiguous array, so walking them for depth or a sweep is a linear scan. The worst case is an
insert or erase deep in the book, which moves every level above it (24 bytes each): O(levels). A `std::map` never
moves anything but chases a pointer per level; a price-indexed array is O(1) but prices span 1 to 10^9.

**How does the id table delete without tombstones?**
Linear probing with backward-shift deletion (Knuth's Algorithm R, `IdTable::take`). After emptying a slot, walk the
rest of the cluster: an entry may move back into the hole unless its home slot lies cyclically after the hole and
at or before its own slot (moving it back would put it before where its probe starts). Each entry that moves leaves
the new hole. So a probe can always stop at the first empty slot, and lookups never slow down as orders come and go.
The random test against `std::unordered_map` runs on a 16-slot table where clusters wrap past the end; the three
mutations of that condition (move everything, move nothing, ignore the wrap) each fail it.

**Why Fibonacci hashing?**
Ids increase by one. `id * 2^64/φ`, keeping the top bits, sends consecutive ids far apart, so an increasing run of
ids doesn't fill one cluster; it costs one multiply and one shift, and the table's size being a power of two makes
the shift the modulo.

**How do you know the engine doesn't allocate?**
`tests/allocation_test.cpp` replaces the global `operator new` with a counter and counts the calls while a book
applies 200,000 random commands: 0 for the engine, in four mixes. A count of 0 could also mean a broken counter, so
day 2's engine runs the same commands as the control: 169,627 to 211,721 allocations. Past its room (4,096 orders by
default, on a stream that reaches 7,670), the engine made 4 allocations in 100,000 commands: the pool, the index and
both sides' levels growing once each. Removing the free list or any of the three reservations fails the test.

**Why write the move constructor yourself?**
The defaulted one moves the vectors (leaving them empty) but copies the free list's head, so a moved-from book would
take its next node from an index into a pool it no longer has: a crash, as the mutation showed. The written moves
reset it, so a moved-from book is empty and usable. The move assignment also clears the source's vectors, because the
standard only promises "valid but unspecified" there; libc++ empties them anyway, so no test here can fail that line.

**How does the ring work without locks?**
Two counters, each written by one thread only: the producer's push count and the consumer's pop count. The producer
writes the slot, then stores its count with release; the consumer loads it with acquire, so the slot's contents are
visible before it reads them. Freeing is the mirror image. Each side caches the other's count and only reloads it when
the ring looks full (or empty), so the common case touches no shared line but the slot. The counters only increase,
so there is no ABA: a slot is the count modulo the size.

**Why 128-byte alignment?**
False sharing: if the two counters shared a cache line, each thread's write would take the line from the other's
core every time. This M3 reports `hw.cachelinesize` 128, and Intel cores prefetch 64-byte lines in pairs, so 128
keeps them apart on both.

**What did ThreadSanitizer miss?**
A probe outside the repo: TSan reported a ring of `uint64_t` whose push published with a relaxed store, and stayed
silent on the same bug with 64-byte struct values, which the compiler copies with `memcpy`. So the ring's threaded
tests run on both: integers, where TSan sees every one of the three memory-order mutations, and structs whose fields
must agree, which on the M3's weak memory model caught a relaxed load of the tail as torn values even without TSan.

**What happens if the sender stops reading reports?**
The engine fills the ring of reports, waits for room and takes no more requests; then the ring of requests fills and
`try_send` fails. Both threads wait: so the rule is that the sender keeps receiving while it waits to send. `stop()`
copes with a sender that has given up: the engine checks the stop flag while it waits for room and leaves. A test
drives exactly that state (it is the only state in which 2 × 16,384 + 1 sends can succeed) and checks that `stop()`
returns with 16,385 commands applied.

**Can a request sent just before `stop()` be lost?**
The engine looks at the flag only when the ring is empty, and then looks at the ring once more. The flag is stored
with release after the sender's last push, and the engine loads it with acquire, so if it sees the flag it also sees
that push. No test can catch the flag stored relaxed (the window is a few instructions), so the argument is the
guarantee; a mutation run with it relaxed passed.

**How do you measure latency, and what is coordinated omission?**
`bench/latency.cpp` times each command alone (a clock read either side), and the engine's thread from the sender's
side. In the paced runs each command is due at start + i/rate and its latency is measured from when it was due, not
when it was sent: if the sender is held up (the ring is full), the commands behind it would otherwise be timed from
their late sends and the stall would vanish from the percentiles. That is coordinated omission. The clock here
(`steady_clock`) steps in 41 to 42 ns (a 24 MHz timer) and costs 13 ns a read, so the per-command figures are in
steps: the median is one step, and only the tail spans several.

**What did day 3 buy, in numbers?**
`make bench` at `2e54e0d` (on battery, 5 repetitions, medians): 82 to 92 M commands a second from 7 to 583 resting
orders, 58 M at 5,410, against 19 to 24 M for day 2's engine in the same run: 3.8 to 4.5 times, and 3.0 at 5,410. On
a 7-order book it now edges the reference model's vector scan (1.07 times; day 2's was 0.24). In `make latency` its
p99.99 is 167 to 209 ns at every size, against 833 to 958 ns for day 2's, whose allocations show in the tail.

**What does a trip through the engine's thread cost?**
One command at a time, from the sender's `try_send` to its last report: 166 to 167 ns at the median, 250 to 459 ns at
p99, a few cache-line handoffs between cores each way plus the command. Paced at 1 M or 5 M a second the median is about
200 ns, but p99.9 jumps to 26 to 35 µs. Counting the paced commands sent more than 5 µs late showed about half of the
slow ones were held up on the sender's thread and the rest after it: both spinning threads stall for tens of
microseconds now and then (unpinned, on macOS), and timing from when each command was due charges each stall to every
command due during it. One at a time, a stall delays only the command in flight, which is why that mode's tail is so
much smaller. Saturated, 13.3 to 13.8 M commands a second: the measuring sender is the bottleneck, and the tail is
queueing.

**What did the review of day 3 find?**
No bug in 8.4 million edge-case commands against the reference model (books built with room for 0 to 2 orders, prices
at both bounds, ids near 2^64, copies and moves mid-stream) or 6 million id-table operations with colliding homes. Two
of its hypotheses reproduced as failing tests here: a copy of a book had no room (vector copies keep only what they
hold: 36 allocations on 200,000 commands), and a failed allocation could leave an empty level (`rest` made the level,
then the pool failed to grow). Both are fixed; the second led to the rule that every event is appended before the
change it reports, and to a test that fails each allocation of each command in turn. Its surviving mutations: `take(0)`
without its guard (now tested), and lines no test can see (the stop protocol's second look, the source's `clear()`s
after a move).

**Why not catch the exception on the engine's thread?**
If applying a command throws, the book is well-formed, but that command was cut short: an order whose remainder was to
rest doesn't, with no event saying so. An engine that keeps matching after that has clients holding a wrong picture of
their orders. So the exception leaves the thread and the process terminates (fail-stop); within its room the engine
allocates nothing, so it can only happen past the room a deployment chose.

## The gateway and market data (day 4)

**How does an order get from a client to the book and back?**
The client writes a line, `new 7 sell limit 10 @ 101`, on a TCP connection to port 8107. The gateway's thread reads it
(`poll`, then `recv`), parses it with the scenario files' parser, checks the session's id rule, gives the order the
engine's next id (say 42), records the route (42 belongs to this session as 7) and pushes `Request{sequence, new 42
...}` into the engine's ring. The engine's thread pops it, applies it to the book and pushes each event as a `Report`,
the last one flagged. The gateway pops those, turns each into the client's line (`accepted 7`, `fill 7 4 @ 101`),
follows the command in the depth feed, and writes the lines to the socket. Two threads, two rings, no locks.

**Why does the gateway assign the engine's ids instead of using the client's?**
The engine needs ids that increase across everyone (that is how it rejects a duplicate with one comparison), and
clients don't coordinate. So each client keeps its own ids, which must increase within its session (the same rule,
checked by the gateway), and the gateway maps them to one increasing sequence. It is also what keeps one client's ids
from colliding with another's, and from leaking: a trade is sent as a `fill` to each side under that side's own id.

**How do you keep a session's answers in order when the gateway answers some lines itself?**
The engine answers commands in the order they were sent, so its answers for a session already come in line order.
The gateway's own answers (`rejected 9 unknown-order` for an id it doesn't know, a `book` snapshot, an `error`) would
jump the queue if written at once, so they are held: each records how many of the session's commands were in the
engine when it was made (`Held::after`) and is written once that many have been answered (`flush_held`, called from
`command_answered`). A held snapshot is taken when it is written, so it includes the commands before it. The review
found the one answer that skipped this, the long-line error.

**What happens when a client disconnects with orders resting?**
They are cancelled. When a session ends (it closed its side and everything is written, or a socket error, or it
stopped reading), `end_session` queues a cancel for each order still live, and the loop sends them as the gateway's
own commands, whose answers go nowhere. The review found that the loop could fall asleep right after queueing them:
`busy` had been worked out before. A test now checks that a disconnect sends exactly one cancel per live order and no
more, by the snapshot's sequence number, which also catches any route the gateway forgot to forget.

**What happens when the engine's ring fills, or a client stops reading?**
A full request ring stops the handling of lines (the gateway keeps draining the reports, so the engine never waits
on it for long) and the loop comes back without sleeping. A client whose unhandled input passes 1 MiB isn't read, so
its sends back up into TCP. A client that doesn't read its answers is dropped once 4 MiB of them are waiting in the
gateway; its test had to send until the connection broke, because how much the kernels buffer on the way differs (a
megabyte on Linux, tens of megabytes here).

**Why one thread with poll instead of a thread per connection?**
The engine's thread takes exactly one sender (single-producer ring), and one thread owning every session means no
shared state in the gateway at all. Its cost is that one slow step (a big snapshot) delays every session; a session
handles at most 256 lines a turn so one client can't starve the others.

**How is the L2 feed built, and how do you know it's right?**
From each command and its events, not from the engine: `DepthFeed` keeps its own record of resting orders and level
totals. An accepted limit order's remainder rests at its price; a trade takes from the maker at the maker's price; a
cancellation takes what was left. Each command's changed levels are listed once (a taker never returns to a level, so
comparing with the last one is enough). It's checked three ways: after every command of 110,000 random commands, the
feed's levels and a subscriber's equal the book's depth; events that don't fit its record throw (the tool built
around the test kit's deliberately broken price-bug book fails `replay` at the command `diff` finds); and through the gateway, a subscriber's
book equals the gateway's snapshot after 5,000 random commands.

**What's a sequence gap and how does a subscriber recover?**
Every command the engine answers gets the feed's next number, and a subscriber gets an `update` line for each, even
one that changed nothing (`update 9`). `L2Book::apply` accepts only the number after the last one it applied; a gap
or a repeat is refused, and the subscriber starts again from a fresh snapshot, which carries its sequence number:
the snapshot-and-recovery pattern of exchange feeds. Over one TCP connection nothing is lost, so this matters for
joining (`subscribe` sends the snapshot, then the updates after its number) and would matter over UDP.

**How did you measure the gateway, and what did you find?**
`orderbook_loadgen` times each command from when it was due to its acknowledgement, over TCP: one at a time 23 to 28
µs at the median, almost all of it sockets (the engine's thread alone answers in 166 ns); batched, 2.70 to 2.74 million
commands a second. Paced one write a command, the median stays near 30 µs at every rate, but from 100,000 a second the
p99 is in milliseconds and grows with the rate. The sender wasn't late, so commands waited after leaving it: some stall
of milliseconds on the path, which the timing charges to every command due during it. I haven't traced it. One
suspect is ruled out: a gateway that spins instead of blocking in `poll` (`--spin on`) had the same p99. The next are
one of three spinning threads being descheduled or moved to an efficiency core, and the load generator's receive path.

**What does the market data cost?**
About 50 ns a command (`BM_EngineAndDepthFeed` against `BM_Engine`): three to five times the engine itself, because
the feed uses a hash map and two sorted maps. It runs on the gateway's thread, next to a parse and socket calls that
cost far more, so it isn't worth the engine's tricks yet; if it were, the engine's own level vectors are the model.

**Tell me about a bug you found today.**
The gateway executable never stopped on SIGINT in its end-to-end test. It blocks SIGINT and SIGTERM and waits for
them with `sigwait`; but a non-interactive shell starts `&` jobs with SIGINT *ignored*, and an ignored signal is
discarded, never left pending, so `sigwait` never returned and the test hung until CTest's timeout. The fix resets
both dispositions to the default before blocking them. Another: CI's GCC 14 on Linux flagged `errno == EAGAIN ||
errno == EWOULDBLOCK` (they are equal there, and glibc's `errno` is a const function GCC can compare), which GCC on
macOS can't see.

**What would you do next with the gateway?**
Trace the paced tail (where the milliseconds go), then a binary protocol and a batched write per turn per session,
pinning on Linux, and pushing market data over UDP multicast with the gap recovery the subscriber already has.

## The latency histograms and the release (day 5)

**What did day 5 add?**
Histograms of the latencies both measuring tools already timed (`--csv FILE` on `orderbook_latency` and
`orderbook_loadgen`), a script that draws them (`scripts/plot_latency.py`, `make histograms`), a per-command trace for
the gateway's load generator (`--trace FILE`) with a script that says when the slow commands were due
(`scripts/trace_tail.py`), `orderbook version`, the release docs and v1.0.0. No change to the engine, its thread or
the gateway.

**Why these buckets?**
They are HdrHistogram's layout: one bucket a nanosecond below 64 ns, then 32 to each doubling (`bucket_of` takes the
doubling from the leading zeros and the top six bits as the bucket within it). So every bucket is at most 1/32 (3.1%)
of its lower bound wide, at 50 ns and at 50 ms alike, the bounds are whole nanoseconds, and 2^63 ns needs under 2,000
buckets. Fixed-width bins would be too coarse at the small end or far too many at the large end; bins a tenth of a
decade wide would have fractional bounds and several empty bins below 10 ns.

**You keep every sample anyway. Why a histogram?**
The tables' percentiles do come from the sorted samples, exactly (`at_quantile`, nearest rank). The histogram is the
form that goes in a file and a figure: 1,000,000 samples become a few hundred buckets, two runs' files can be added
bucket by bucket, and the shape (a mode, a second mode, a flat tail) is visible in it. HdrHistogram exists for
programs that can't keep their samples; this one can, so it keeps both.

**How do you read the percentile figures?**
The x axis is 1/(1 - p) on a log scale, so 90%, 99%, 99.9% and 99.99% are evenly spaced: each step right is a ten
times rarer command. The y axis is latency, also log. A flat line means most commands cost the same; a cliff is where
a different cause takes over; a line ends at its slowest command. In the gateway's figure (run 1), the paced lines stay under 100 µs for 99.2% of the
commands at 50k/s, 98.1% at 100k/s and 95.4% at 200k/s, then climb two to three decades: the last 1 to 5% are a
different population.

**How did you find where the gateway's tail came from?**
Its p99 at 200,000 commands a second was 16 to 17 ms in all three runs, and the maximum 19.7 to 21.1 ms (day 4's runs:
15.5 to 18.0, 19.0 to 21.4), with the sender on time. Chance stalls don't repeat like that. So I traced every command
(`--trace`: row, index, connection, due time, latency) and grouped the slow ones by when they were due
(`trace_tail.py`): in every paced row the worst episode starts as the row starts. At 200k/s, the 7,310 commands over
1 ms due in the first 37 ms include the row's slowest (20.2 ms); the first five commands of each row took 149 to 493
µs against medians of 32 to 42 µs, so a backlog builds while the start is slow and drains after. Without each row's
first 50 ms, the p99 at 200k/s is 1.8 ms, not 16.2.

**Isn't leaving out the first 50 ms cheating?**
It would be if it replaced the full numbers; it doesn't. The README's tables and figures are every command from the
first, which is what a client that connects and starts sending sees. The cut numbers answer a different question:
what the steady state costs. The honest next step is an untimed paced lead-in before the timed commands, which
measures the steady state directly instead of cutting a run afterwards.

**So what makes the start of a row slow?**
Not known yet. Each row opens fresh connections after a 200 ms pause (in which the gateway cancels the last row's
orders) and an untimed saturated warm-up of 8,000 commands, then paces. Ruled out: the gateway's wakeup from `poll`
(day 4: spinning gave the same p99). Left: the scheduler running threads that idled through the pause on efficiency
cores or at a low clock until they have run for a while (macOS has no thread affinity on Apple silicon), and the new
connections. To separate them: keep the connections across rows, drop the pause, add the lead-in, and set the threads'
QoS class; on Linux, pin the threads and fix the CPU frequency.

**How do you know the figures show what they say?**
Each point is a bucket at its lower bound, so it is within 3.1% below the exact value in the table. A CTest test runs
both tools briefly, draws all four figures from their real files and parses each back as XML; it fails if one isn't
drawn. And I read the first figures against the tables, which is how the first bug below was found.

**Tell me about a bug you found today.**
The first figures said "1,000 resting orders" for a book of 583. The CSV's size column held `RandomFlow`'s
`target_resting` (100, 1,000, 10,000), which the stream approaches but doesn't reach (70, 583, 5,410 after the
warm-up). The column now holds the orders resting after the warm-up (the same on every book, since they agree event
for event), and the script takes each figure's size from the data instead of naming one.

**What does v1.0.0 promise?**
CHANGELOG.md says it: the rules in DESIGN.md (validation and its order, matching, the events and their order), the text
formats of commands, events and scenario files, the gateway's protocol, the tool's commands and exit codes, and
`Book`'s interface. A change to any of those is a major version. `orderbook version` prints the version from
`project()` in CMakeLists.txt, through a header CMake generates, so it has one source.

**Every number is from a laptop on battery. How much does that matter?**
Less for the medians, which repeat from run to run (the engine's thread 167 ns one at a time in all three runs, the
gateway 29 to 30 µs), than for the tails, which move by up to ten times (the engine thread's one-at-a-time max: 10.5 to
126.7 µs). So the README reports three runs, not the best one, names the run the figures come from, and labels every
result with the power and the load. The comparisons that matter, such as one book against another, are made in the
same run.

## Testing

**Why write a slow reference model at all?**
Because a fast engine's own unit tests only check the cases someone thought of. A reference model that is the
specification written as directly as possible can be checked by reading, and then the engine can be compared with
it on millions of random commands, every event. The reference is O(n) per fill: it scans a whole side to find the
best order. That's fine for an oracle and is never optimised.

**How do you know the reference model is right?**
Three independent ways. It is short enough to read against DESIGN.md line by line (the comments name the rules).
The fifteen scenario files were written by hand from DESIGN.md, not recorded from the code, so they encode what the
rules say, not what the code does. And `Audit` (`src/check.cpp`) checks every event of long random runs against its
own record of the resting orders, sharing none of the reference's matching code: 160,000 random commands in
`check_test.cpp`.

**What does the Audit check that the scenario files can't?**
Scale and variety. A scenario file checks the cases its author thought of. The Audit checks every command of a
random run: each trade is with the best order by price and then arrival (I2), at the maker's price and within the
taker's limit (I3), for the smaller remaining quantity (I4); a taker stops only when filled or no longer crossing;
cancels report what was left; and the events come in the specified order. `compare(book)` then checks that the
book holds exactly the orders the events imply, in priority order.

**How do you know the checks can fail?**
Each check has a test that feeds it something wrong and expects it to complain: `Audit.CatchesEachBrokenRule` gives
it nine event streams that each break one rule (a trade with the second order in a queue, at the wrong price, for
the wrong size, a taker that stops early...), and `CheckBook.CatchesEachKindOfBadState` gives `check_book` ten bad
states. The CLI test for a wrong scenario file matches the report's text, so a crash wouldn't count as a pass.

**Why not use `std::uniform_int_distribution` for random commands?**
The standard specifies the generators' output (`std::mt19937_64` is the same everywhere) but not the
distributions'. libstdc++ (Linux) and libc++ (macOS) turn the same random bits into different numbers, so the same
seed would give different command streams on the two CI platforms, and a failure on Linux couldn't be replayed on
the Mac. `RandomFlow` uses SplitMix64 and its own unbiased bounded draw (`below`: reject draws under 2^64 mod n).
Each draw is its own statement, because the order a function's arguments are evaluated in is unspecified too: GCC
often evaluates right to left.

**How is that determinism actually verified?**
`RandomFlow.TheStreamIsTheSameOnEveryPlatform` hashes seed 1's first 100,000 commands and compares with a pinned
value, and `Invariants.TheReferenceModelsEventsAndBookAreTheSameOnEveryPlatform` does the same for every event the
reference model produces on 50,000 commands and the book it ends with (I7). CI runs both with GCC and Clang on Linux,
Apple Clang on macOS, and under the sanitizers, in Debug and Release; both passed on all four jobs the first time
they ran there.

**Tell me about a bug you found in your own test tooling.**
The Audit, the checker meant to catch a wrong engine, would have passed one. Its trade loop compared each trade's
quantity with the smaller of the taker's and the maker's remaining quantities, but never checked that the taker had
any left. After a taker was filled, the expected fill was `min(0, ...)` = 0, so an engine that emitted
`trade 3 2 0 @ 101` after the real fills, any number of times, passed. The day-1 review found it by asking what wrong
event streams the Audit would accept; the fix is one line at the top of the loop (`src/check.cpp`), and the review's
two tests are kept in `tests/review/`. The reference model never emitted such events, so no earlier result changed.

**How do you know the tests test anything?**
Mutation checks: change one mechanism (delete a check, swap two comparisons) and see whether any test fails. The
review made 67 such changes; four went unnoticed (the Audit's limit check and its quantity-before-price order,
`check_book`'s best-price check, RandomFlow's mid clamp), and each now has a test that fails without it. A
reference model whose rejections use up the id now fails 17 tests (the second review's count at `aa0599b`: 14
GoogleTest tests, two CLI tests and the book fuzzer's run), among them all four random invariant runs, because
the random stream sends some rejected orders again under the same id. One trap: a mutant that doesn't compile tests
nothing. Deleting the best-price check left unused variables, `-Werror` stopped the build, and the old binary ran
instead; the rerun disabled the condition.

**What does the minimizer do?**
`minimize` is ddmin: given a failing stream and a function that replays a stream from scratch and says whether it
still fails, it drops chunks while the failure persists, then smaller chunks, down to single commands, so every
command left is needed. In the tests a deliberately broken book (a trade a tick off when its maker had traded before)
fails a 3,000-command random run, and 32 replays leave three commands: a resting sell, an IOC buy that part-fills it,
and a market buy that trades with it again. The differential harness uses it to report engine-versus-reference
disagreements.

**How is the engine tested against the reference model?**
`Lockstep<ReferenceBook, Book>` applies each command to both and compares the two event vectors; at checkpoints
`state_difference` compares each side's orders (id, price, remaining, in priority order), depth and best price, the
number resting and the last accepted id. `differential_test.cpp` runs seven mixes, 720,000 commands, from the default
mix to one-lot orders (every fill empties its maker), three-tick ranges (long queues), quantities to 10^9 on a nearly
empty book, 30% spoiled orders, a deep book with 900+ levels a side, and prices at both bounds; each mix must reach a
minimum book size, so a stream that stopped building a book fails. The engine's events also go through the Audit and
its state through `check_book`, and `orderbook diff SEED COUNT` runs any number more (1,000,000 in 3.7 s, Debug).

**What does a failing differential test print?**
A scenario file: the shrunk commands, each with the reference model's events, a comment where the engine's differ, and
at the end the expected depth and the rest of the state (each side's orders and best price, the number resting, the last
accepted id), so the engine fails the file whichever part differed. Breaking the engine on purpose (new orders to the
front of their level) gives three commands: two asks at one price, then a cancel of the second, which the broken engine
reports with the first one's quantity, because its index pointed at the wrong node. The broken engine fails the file;
saved under `tests/scenarios/`, it is a regression test.

**What if the engine has a memory error? Would the differential tests see it?**
Usually, but not always as a difference. Running `make soak` on the time-priority mutant (whose index pointed at the
wrong list nodes, so cancels freed nodes other entries still pointed to), all 24 runs failed, but three crashed. Two
crashed while shrinking, which replays the broken book hundreds of times, so `diff` now prints the difference before
it shrinks. One crashed before any event differed: two makers had the same quantity, so a cancel of the wrong node
produced the right event, and a later cancel used a freed node. Undefined behaviour can't be diffed reliably; that is
what the sanitizer build is for, and it named the line (`build/asan/orderbook diff 2 ...`: heap-use-after-free in
`Book::cancel`). CI runs every differential test under ASan and UBSan for that reason.

**Tell me about a bug in the differential harness.**
It came from that same deliberate mutation. The first version shrank by "the books differ in any way", events or
final state. A wrong queue order shows in the state one command before it shows in an event, so ddmin kept the two
resting orders and dropped the cancel: a file whose events all agree, which the broken engine passes. Now
`Difference` records whether events differed, and the shrinking keeps that kind (`cabaf95`); a test with a book whose
time priority is backwards (`BookWithNewestFirstQueues`) gives two commands under the old rule and three under the
new, in a file the broken book fails.

**What did the review of day 2 find?**
A background agent reviewed the day's code at a fixed commit, as a correctness review in its own worktree, and saved
each bug it reproduced as a failing test. In the engine, nothing: an edge-case script (ids 0 and 2^64 - 1, bounds and
one past them, market orders with a price) and eight unusual random mixes agreed with the reference model. In the
harness, one real bug: a difference only in a book's best price, its count or its last accepted id shrank to a
scenario file the broken book passed, because the file checked events and depth only. Scenario files now have state
checks, and every harness-written file ends with all of them. And 13 of its 39 mutations went unnoticed or were caught
only by a crash, among them every way the CLI could get `--book` or `diff`'s exit code wrong, since both books print
the same output; the tool is now also built, for tests only, around deliberately broken books. A second agent then
reviewed only the code written after the first, fixes included, and found two more: an order on the wrong side was
caught but reported as two identical strings (the side isn't in the line form, so such reports now print it), and the
soak script left its runs going on Ctrl-C and its temp directory behind under dash.

**How is the code fuzzed?**
Two libFuzzer targets in `fuzz/`: arbitrary bytes into the text parsers (a line that parses must format and parse back
to itself), and bytes decoded into commands on 16 prices for both books in lockstep, their events and states compared
after every command, with the Audit and `check_book` on the engine. CI runs each for 60 s under ASan and UBSan; Apple
Clang has no libFuzzer, so on the Mac and in the other jobs a small `main()` runs them on the scenario files and on
seeded random inputs.

## Build and CI

**What did CI catch that the Mac didn't?**
Four GCC-only warnings, each an error under `-Werror`, in four red runs of the GCC job:
- A lambda parameter named `info` shadowed the parameter of the function `INSTANTIATE_TEST_SUITE_P` wraps it in
  (`-Wshadow`). The test names now come from a named function.
- `if (...) ASSERT_EQ(...)` without braces: GoogleTest's assertions expand to an if-else (`-Wdangling-else`).
- `static_cast<int64_t>(state.iterations())`, already an `int64_t` (`-Wuseless-cast`). This kind is
  platform-dependent: `int64_t` is `long` on Linux and `long long` on macOS, and `uint64_t` and `size_t` are the same
  type on Linux only, so a cast between them is a no-op there and nowhere else. I removed one such cast before it
  failed and missed this one.
- A "potential null pointer dereference" inside libstdc++'s `streambuf`, in the release build only, inlined from a
  `std::istreambuf_iterator` read in the CLI (`-Wnull-dereference`). It's a false positive; the tool now reads files
  with `out << in.rdbuf()`, as the tests already did, so the warning stays on.
CI's GCC 14 job is the only place the tests run under GCC. Later the same day I found Homebrew's GCC 15 on this Mac;
`make gcc-check` (`scripts/gcc_check.sh`) now compiles every file of the project with it, Debug and Release, before a
push. It can't run the tests (GoogleTest doesn't build with GCC against the macOS SDK), but it catches these
warnings. The commits that broke the GCC job and the ones that fixed it are in the history; CI has been green on all
four jobs since `f8e280e`.

**Why pin the dependencies and the runners?**
So a commit always builds the same way. GoogleTest and Google Benchmark come from release tarballs checked against
SHA-256 hashes (`cmake/Dependencies.cmake`); the runners are `ubuntu-24.04` and `macos-15`, not `-latest`, which
GitHub moves to Ubuntu 26 from October 19.

**What do the benchmark numbers mean?**
`BM_ReferenceBook/N` fills a book to about N orders with a random stream, then times the same 10,000 commands against a
fresh copy of it. At 582 resting orders the reference does 4.54 M commands/s; at 5,417, 535 k: nine times the orders,
8.5 times the cost per command, linear as a scan should be. `BM_Engine/N` and `BM_MapBook/N` time the same commands
against the same book (each iteration rebuilds the book from the warm-up with the timer paused, since day 2's engine
can't be copied): on day 3, 10.9 to 12.1 ns a command for the engine up to 583 orders and 17.3 ns at 5,410, against 42
to 52 ns for day 2's. All were measured on battery with other apps open (day 2's first run during a video call), so
treat them as rough; the comparison between books in the same run is the reliable part.

## Limits

**What doesn't it handle?**
One instrument per book; no order modification (cancel and re-enter loses priority, which is also what most
exchanges do for a price change), no self-trade prevention (the engine has no notion of accounts), no fill-or-kill,
stop or iceberg orders, no auctions, fees or persistence, and prices must be positive.
