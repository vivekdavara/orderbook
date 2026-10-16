include(GoogleTest)

add_executable(orderbook_tests
  text_test.cpp
  types_test.cpp)
target_link_libraries(orderbook_tests PRIVATE orderbook::orderbook GTest::gtest_main)
orderbook_set_warnings(orderbook_tests)

# Each TEST becomes its own CTest test, so a failure is named in ctest's summary.
gtest_discover_tests(orderbook_tests DISCOVERY_TIMEOUT 30)
