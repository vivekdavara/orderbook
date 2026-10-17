add_executable(orderbook_bench
  book_bench.cpp)
target_link_libraries(orderbook_bench PRIVATE orderbook::testkit benchmark::benchmark_main)
orderbook_set_warnings(orderbook_bench)
