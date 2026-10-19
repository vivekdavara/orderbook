add_executable(orderbook_bench
  book_bench.cpp)
target_link_libraries(orderbook_bench PRIVATE orderbook::testkit benchmark::benchmark_main)
orderbook_set_warnings(orderbook_bench)

# Latency percentiles: each command timed alone in one thread, and the engine's thread timed from the sender's side
# (make latency).
add_executable(orderbook_latency latency.cpp)
target_link_libraries(orderbook_latency PRIVATE orderbook::testkit)
orderbook_set_warnings(orderbook_latency)

# The TCP gateway's latency and throughput from the clients' side (make loadgen); without --port it runs a gateway in
# its own process.
add_executable(orderbook_loadgen loadgen.cpp)
target_link_libraries(orderbook_loadgen PRIVATE orderbook::testkit orderbook::gateway)
orderbook_set_warnings(orderbook_loadgen)
