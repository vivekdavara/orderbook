# Walkthrough

Questions an interviewer is likely to ask about this project, with short answers grounded in the code. Day 1 covers
the specification, the reference model and the test kit; later days add the engine, the gateway and the
measurements.

Reading order for day 1: DESIGN.md ("Validation", "Matching", "Events", "Invariants"), then
`src/reference_book.cpp` (one screen), a scenario file such as `tests/scenarios/03_time_priority.txt`, then
`src/check.cpp` (`Audit::record_new`) and `src/random_flow.cpp`.

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

## Testing

**Why write a slow reference model at all?**
Because a fast engine's own unit tests only check the cases someone thought of. A reference model that is the
specification written as directly as possible can be checked by reading, and then the engine can be compared with
it on millions of random commands, every event. The reference is O(n) per fill: it scans a whole side to find the
best order. That's fine for an oracle and is never optimised.

**How do you know the reference model is right?**
Three independent ways. It is short enough to read against DESIGN.md line by line (the comments name the rules).
The fourteen scenario files were written by hand from DESIGN.md, not recorded from the code, so they encode what the
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
the wrong size, a taker that stops early...), and `CheckBook.CatchesEachKindOfBadState` gives `check_book` nine bad
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
reference model whose rejections use up the id now fails 13 tests, among them all four random invariant runs, because
the random stream sends some rejected orders again under the same id. One trap: a mutant that doesn't compile tests
nothing. Deleting the best-price check left unused variables, `-Werror` stopped the build, and the old binary ran
instead; the rerun disabled the condition.

**What does the minimizer do?**
`minimize` is ddmin: given a failing stream and a function that replays a stream from scratch and says whether it
still fails, it drops chunks while the failure persists, then smaller chunks, down to single commands, so every
command left is needed. In the tests a deliberately broken book (a trade a tick off when its maker had traded before)
fails a 3,000-command random run, and 32 replays leave three commands: a resting sell, an IOC buy that part-fills it,
and a market buy that trades with it again. Day 2 uses it to report engine-versus-reference disagreements.

**How is the code fuzzed?**
Two libFuzzer targets in `fuzz/`: arbitrary bytes into the text parsers (a line that parses must format and parse back
to itself), and bytes decoded into commands on 16 prices for the reference model under the Audit and `check_book`
after every command. CI runs each for 60 s under ASan and UBSan; Apple Clang has no libFuzzer, so on the Mac and in
the other jobs a small `main()` runs them on the scenario files and on seeded random inputs.

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
This Mac has no GCC, so CI is the only place these show up. The commits that broke the GCC job and the ones that
fixed it are in the history; CI has been green on all four jobs since `f8e280e`.

**Why pin the dependencies and the runners?**
So a commit always builds the same way. GoogleTest and Google Benchmark come from release tarballs checked against
SHA-256 hashes (`cmake/Dependencies.cmake`); the runners are `ubuntu-24.04` and `macos-15`, not `-latest`, which
GitHub moves to Ubuntu 26 from October 19.

**What do the benchmark numbers mean?**
`BM_ReferenceBook/N` fills a book to about N orders with a random stream, then times the same 10,000 commands
against a fresh copy of it. At 582 resting orders the reference does 4.54 M commands/s; at 5,417, 535 k: nine times
the orders, 8.5 times the cost per command, linear as a scan should be. They are the baseline for day 2's engine,
measured on battery with other apps open, so treat them as rough.

## Limits

**What doesn't it handle?**
One instrument per book; no order modification (cancel and re-enter loses priority, which is also what most
exchanges do for a price change), no self-trade prevention (the engine has no notion of accounts), no fill-or-kill,
stop or iceberg orders, no auctions, fees or persistence, and prices must be positive.
