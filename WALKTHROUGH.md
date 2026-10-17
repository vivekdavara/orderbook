# Walkthrough

Questions an interviewer is likely to ask about this project, with short answers grounded in the code. Day 1 covers
the specification, the reference model and the test kit; day 2 the engine and the differential tests; later days add
the fast engine, the gateway and the measurements.

Reading order for day 1: DESIGN.md ("Validation", "Matching", "Events", "Invariants"), then
`src/reference_book.cpp` (one screen), a scenario file such as `tests/scenarios/03_time_priority.txt`, then
`src/check.cpp` (`Audit::record_new`) and `src/random_flow.cpp`.

Reading order for day 2: `include/orderbook/book.hpp` and `src/book.cpp` (`new_order`, `rest`, `cancel`), then
`include/orderbook/differential.hpp` (`Lockstep`, `first_difference`, `shrink_to_scenario`), then
`tests/differential_test.cpp` (`expect_same_books` and the seven mixes).

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

**How is the engine built?**
`Book` (`src/book.cpp`) keeps each side as a `std::map` from price to level, best first, so the best price is
`begin()`. A level is a `std::list` of `{id, remaining}` in arrival order plus its total quantity. An
`std::unordered_map` from id to `{side, map iterator, list iterator}` finds a resting order without searching.
A taker walks the opposite map from `begin()` while it crosses, trades with each level's queue from the front, erases
filled makers from the queue and the index, and erases a level when it empties; a limit remainder is pushed to the
back of its level (`try_emplace` makes the level if it's new) and indexed.

**What does each operation cost?**
Resting a new order is O(log L) for its level (L levels on that side) and O(1) to queue and index it. Each fill is
O(1), and erasing an emptied level by iterator is amortised O(1). A cancel is O(1) amortised: index lookup, list
unlink, and a level erase by iterator if it emptied. Best price is O(1), depth O(L), since each level keeps its total.

**Why one comparator type for both sides?**
`BetterPrice{side}` compares higher-first for bids and lower-first for asks. With `std::greater` and `std::less` the
two maps would be different types, so the index would need two iterator types (a variant, or two indexes) and the
matching code would become a template. One type keeps one `Location` struct and one code path; the cost is a
well-predicted branch per comparison, and day 3 replaces the map anyway.

**Why is `Book` move-only?**
Its index holds iterators into its own map and lists. A defaulted copy would copy those iterators, so the copy's
cancels would unlink nodes in the original: a use-after-free once the original goes. Copying would need the index
rebuilt for the copy's nodes, and nothing needs a copy, so the copy constructor is deleted (`book_test.cpp` asserts
it). A move is safe: node-based containers hand their nodes to the new container, so every iterator still refers to
the same element, now in the moved-to book; a test moves a book and then cancels and trades in it, under ASan too.

**Why is the engine slower than the reference model on small books?**
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

**Is the engine deterministic, with an `unordered_map` inside?**
Iteration order of an `unordered_map` differs between standard libraries, but the engine never iterates its index:
it only looks ids up. Everything that decides events or the reported state walks the maps and lists, whose order is
the book's. A test requires the engine's events and final book on 50,000 commands to hash to the reference model's
pinned value, on every CI job (GCC, Clang, Apple Clang; Debug, Release, sanitizers).

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
the same output; the tool is now also built, for tests only, around deliberately broken books.

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
`BM_ReferenceBook/N` fills a book to about N orders with a random stream, then times the same 10,000 commands
against a fresh copy of it. At 582 resting orders the reference does 4.54 M commands/s; at 5,417, 535 k: nine times
the orders, 8.5 times the cost per command, linear as a scan should be. `BM_Engine/N` times the same commands against
the same book (it can't copy one, so it rebuilds the book from the warm-up with the timer paused): 43 to 53 ns a
command from 7 to 5,410 orders. All were measured on battery with other apps open (day 2's during a video call), so
treat them as rough; the comparison between the two books on the same run is the reliable part.

## Limits

**What doesn't it handle?**
One instrument per book; no order modification (cancel and re-enter loses priority, which is also what most
exchanges do for a price change), no self-trade prevention (the engine has no notion of accounts), no fill-or-kill,
stop or iceberg orders, no auctions, fees or persistence, and prices must be positive.
