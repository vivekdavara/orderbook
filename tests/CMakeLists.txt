include(GoogleTest)

add_executable(orderbook_tests
  book_test.cpp
  books_test.cpp
  check_test.cpp
  differential_harness_test.cpp
  differential_test.cpp
  minimize_test.cpp
  random_flow_test.cpp
  review/audit_accepts_zero_quantity_trade_test.cpp
  review/random_flow_unclamped_start_price_test.cpp
  review2/random_flow_double_spoil_breaks_two_rules_test.cpp
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
add_test(NAME cli.options_out_of_range_are_reported COMMAND orderbook_cli gen 1 10 --range 600000000)
set_tests_properties(cli.options_out_of_range_are_reported PROPERTIES
  PASS_REGULAR_EXPRESSION "orderbook: price_range must be 0 to 499999999, got 600000000")
add_test(NAME cli.usage_is_printed_for_bad_arguments COMMAND orderbook_cli gen 1)
set_tests_properties(cli.usage_is_printed_for_bad_arguments PROPERTIES PASS_REGULAR_EXPRESSION "usage: orderbook check")

# The second day-1 review's reproductions of CLI bugs.
include(${CMAKE_CURRENT_SOURCE_DIR}/review2/cli_tests.cmake)

# Exit codes. A test with PASS_REGULAR_EXPRESSION passes whatever the exit code, so each of those has a twin here that
# requires the exact one (0 ok, 1 a failed check or bad input, 2 a usage error).
function(cli_exit_test name code)
  add_test(NAME ${name} COMMAND sh -c "\"$0\" \"$@\" >/dev/null 2>&1; test $? -eq ${code}"
           $<TARGET_FILE:orderbook_cli> ${ARGN})
endfunction()
cli_exit_test(cli.exit.check_of_a_wrong_scenario_is_1 1 check ${CMAKE_CURRENT_SOURCE_DIR}/cli/wrong.txt)
cli_exit_test(cli.exit.check_of_a_missing_file_is_1 1 check ${CMAKE_CURRENT_BINARY_DIR}/no_such_file.txt)
cli_exit_test(cli.exit.run_of_a_bad_command_is_1 1 run ${CMAKE_CURRENT_SOURCE_DIR}/cli/bad_command.txt)
cli_exit_test(cli.exit.bad_arguments_are_2 2 gen 1)
cli_exit_test(cli.exit.options_out_of_range_are_2 2 gen 1 10 --range 600000000)
cli_exit_test(cli.exit.no_arguments_are_2 2)
cli_exit_test(cli.exit.a_passing_audit_is_0 0 audit 5 2000 --target 50)
