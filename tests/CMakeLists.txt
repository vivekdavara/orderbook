include(GoogleTest)

add_executable(orderbook_tests
  check_test.cpp
  minimize_test.cpp
  random_flow_test.cpp
  reference_book_test.cpp
  review/audit_accepts_zero_quantity_trade_test.cpp
  scenario_test.cpp
  text_test.cpp
  types_test.cpp)
target_link_libraries(orderbook_tests PRIVATE orderbook::testkit GTest::gtest_main)
orderbook_set_warnings(orderbook_tests)
target_compile_definitions(orderbook_tests PRIVATE ORDERBOOK_SCENARIO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/scenarios")

# Each TEST becomes its own CTest test, so a failure is named in ctest's summary.
gtest_discover_tests(orderbook_tests DISCOVERY_TIMEOUT 30)

# The command-line tool, end to end.
file(GLOB scenario_files CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/scenarios/*.txt)
add_test(NAME cli.check_passes_every_scenario COMMAND orderbook_cli check ${scenario_files})
add_test(NAME cli.check_fails_on_a_wrong_scenario COMMAND orderbook_cli check ${CMAKE_CURRENT_SOURCE_DIR}/cli/wrong.txt)
# Not just any failure: the report must name the line and show what the book did.
set_tests_properties(cli.check_fails_on_a_wrong_scenario PROPERTIES
  PASS_REGULAR_EXPRESSION "line 4: 'new 2 buy limit 4 @ 105'.*actual events:\n  accepted 2\n  trade 2 1 4 @ 100\n")
# A transcript of a random stream is a scenario file that passes its own check.
add_test(NAME cli.a_transcript_is_a_passing_scenario
  COMMAND sh -c "\"$1\" gen 7 2000 --target 50 | \"$1\" run > \"$2\" && \"$1\" check \"$2\"" sh
          $<TARGET_FILE:orderbook_cli> ${CMAKE_CURRENT_BINARY_DIR}/transcript.txt)
add_test(NAME cli.audit_passes_a_random_stream COMMAND orderbook_cli audit 3 20000 --target 200)
add_test(NAME cli.usage_is_printed_for_bad_arguments COMMAND orderbook_cli gen 1)
set_tests_properties(cli.usage_is_printed_for_bad_arguments PROPERTIES PASS_REGULAR_EXPRESSION "usage: orderbook check")
