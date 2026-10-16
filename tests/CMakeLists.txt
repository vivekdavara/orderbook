include(GoogleTest)

add_executable(orderbook_tests
  check_test.cpp
  random_flow_test.cpp
  reference_book_test.cpp
  scenario_test.cpp
  text_test.cpp
  types_test.cpp)
target_link_libraries(orderbook_tests PRIVATE orderbook::testkit GTest::gtest_main)
orderbook_set_warnings(orderbook_tests)
target_compile_definitions(orderbook_tests PRIVATE ORDERBOOK_SCENARIO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/scenarios")

# Each TEST becomes its own CTest test, so a failure is named in ctest's summary.
gtest_discover_tests(orderbook_tests DISCOVERY_TIMEOUT 30)
