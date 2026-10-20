include(GoogleTest)

add_executable(orderbook_tests
  book_test.cpp
  books_test.cpp
  check_test.cpp
  differential_harness_test.cpp
  differential_test.cpp
  engine_thread_test.cpp
  gateway_test.cpp
  id_table_test.cpp
  latency_histogram_test.cpp
  market_data_test.cpp
  minimize_test.cpp
  random_flow_test.cpp
  review/audit_accepts_zero_quantity_trade_test.cpp
  review/random_flow_unclamped_start_price_test.cpp
  review2/random_flow_double_spoil_breaks_two_rules_test.cpp
  review3/state_only_difference_scenario_test.cpp
  review4/diff_soak_script_test.cpp
  review4/state_mismatch_report_test.cpp
  review5/gateway_review_test.cpp
  scenario_test.cpp
  spsc_ring_test.cpp
  text_test.cpp
  types_test.cpp)
target_link_libraries(orderbook_tests PRIVATE orderbook::testkit orderbook::gateway GTest::gtest_main Threads::Threads)
orderbook_set_warnings(orderbook_tests)
target_compile_definitions(orderbook_tests PRIVATE ORDERBOOK_SCENARIO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/scenarios")
# The measuring tools' histogram (bench/latency_histogram.hpp), header-only.
target_include_directories(orderbook_tests PRIVATE ${PROJECT_SOURCE_DIR}/bench)

# Each TEST becomes its own CTest test, so a failure is named in ctest's summary.
gtest_discover_tests(orderbook_tests DISCOVERY_TIMEOUT 30)

# The engine's promise that matching allocates nothing once the book has room, counted by replacing the global
# operator new: its own executable, since the replacement is process-wide, and not under the sanitizers, which replace
# operator new themselves.
if(NOT ORDERBOOK_SANITIZE)
  add_executable(orderbook_allocation_tests allocation_test.cpp)
  target_link_libraries(orderbook_allocation_tests PRIVATE orderbook::testkit GTest::gtest_main)
  orderbook_set_warnings(orderbook_allocation_tests)
  gtest_discover_tests(orderbook_allocation_tests DISCOVERY_TIMEOUT 30)
endif()

# The command-line tool, end to end. Without --book it runs the engine.
file(GLOB scenario_files CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/scenarios/*.txt)
add_test(NAME cli.check_passes_every_scenario COMMAND orderbook_cli check ${scenario_files})
add_test(NAME cli.check_passes_every_scenario_on_the_reference
  COMMAND orderbook_cli --book reference check ${scenario_files})
add_test(NAME cli.check_fails_on_a_wrong_scenario COMMAND orderbook_cli check ${CMAKE_CURRENT_SOURCE_DIR}/cli/wrong.txt)
# Not just any failure: the report must name the line and show what the book did.
set_tests_properties(cli.check_fails_on_a_wrong_scenario PROPERTIES
  PASS_REGULAR_EXPRESSION "line 4: 'new 2 buy limit 4 @ 105'.*actual events:\n  accepted 2\n  trade 2 1 4 @ 100\n")
# A transcript of a random stream is a scenario file that passes its own check.
add_test(NAME cli.a_transcript_is_a_passing_scenario
  COMMAND sh -c "\"$1\" gen 7 2000 --target 50 | \"$1\" run > \"$2\" && \"$1\" check \"$2\"" sh
          $<TARGET_FILE:orderbook_cli> ${CMAKE_CURRENT_BINARY_DIR}/transcript.txt)
# And across books: the engine passes the reference model's transcript of a stream, and the other way round.
add_test(NAME cli.the_engine_passes_the_reference_models_transcript
  COMMAND sh -c "\"$1\" gen 8 5000 --target 100 | \"$1\" --book reference run > \"$2\" && \"$1\" check \"$2\"" sh
          $<TARGET_FILE:orderbook_cli> ${CMAKE_CURRENT_BINARY_DIR}/reference_transcript.txt)
add_test(NAME cli.the_reference_passes_the_engines_transcript
  COMMAND sh -c "\"$1\" gen 9 5000 --target 100 | \"$1\" run > \"$2\" && \"$1\" --book reference check \"$2\"" sh
          $<TARGET_FILE:orderbook_cli> ${CMAKE_CURRENT_BINARY_DIR}/engine_transcript.txt)
add_test(NAME cli.audit_passes_a_random_stream COMMAND orderbook_cli audit 3 20000 --target 200)
add_test(NAME cli.audit_passes_a_random_stream_on_the_reference
  COMMAND orderbook_cli --book reference audit 3 20000 --target 200)
add_test(NAME cli.diff_finds_the_books_agree COMMAND orderbook_cli diff 4 100000 --target 300 --every 500)
set_tests_properties(cli.diff_finds_the_books_agree PROPERTIES
  PASS_REGULAR_EXPRESSION "^commands 100000: the same events from both books, and the same state every 500 commands")
add_test(NAME cli.options_out_of_range_are_reported COMMAND orderbook_cli gen 1 10 --range 600000000)
set_tests_properties(cli.options_out_of_range_are_reported PROPERTIES
  PASS_REGULAR_EXPRESSION "orderbook: price_range must be 0 to 499999999, got 600000000")
# The version is the one in CMakeLists.txt's project() call, on the first line.
add_test(NAME cli.version_is_the_projects COMMAND orderbook_cli version)
set_tests_properties(cli.version_is_the_projects PROPERTIES
  PASS_REGULAR_EXPRESSION "^orderbook ${PROJECT_VERSION}\nbuilt with ")
add_test(NAME cli.usage_is_printed_for_bad_arguments COMMAND orderbook_cli gen 1)
set_tests_properties(cli.usage_is_printed_for_bad_arguments PROPERTIES
  PASS_REGULAR_EXPRESSION "usage: orderbook .--book engine.reference. check FILE")

# replay: a hand-worked stream gives exactly the expected updates and snapshots; a random stream gives the same
# output on the engine and the reference model (the timing goes to stderr).
add_test(NAME cli.replay_prints_each_commands_updates_and_the_snapshots
  COMMAND sh -c "\"$1\" replay \"$2\" --every 3 --levels 2 --updates | diff - \"$3\"" sh
          $<TARGET_FILE:orderbook_cli> ${CMAKE_CURRENT_SOURCE_DIR}/cli/replay.txt
          ${CMAKE_CURRENT_SOURCE_DIR}/cli/replay_expected.txt)
add_test(NAME cli.replay_is_the_same_on_both_books
  COMMAND sh -c "\"$1\" gen 21 20000 --target 200 > \"$2.commands\" &&
  \"$1\" replay \"$2.commands\" --every 500 --levels 5 --updates > \"$2.engine\" &&
  \"$1\" --book reference replay \"$2.commands\" --every 500 --levels 5 --updates > \"$2.reference\" &&
  cmp \"$2.engine\" \"$2.reference\" && test $(grep -c '^snapshot' \"$2.engine\") -eq 40" sh
          $<TARGET_FILE:orderbook_cli> ${CMAKE_CURRENT_BINARY_DIR}/replay)

# The tool built around a deliberately broken engine (the reference model reporting a trade a tick too high when its
# maker has traded before; broken_books.hpp), to test the tool's own failure paths. diff must report the difference
# and exit 1, and the scenario file it prints must pass on the reference model and fail on the broken engine, which is
# also what runs without --book and with --book engine.
add_executable(orderbook_cli_broken ${PROJECT_SOURCE_DIR}/tools/orderbook_cli.cpp)
target_compile_definitions(orderbook_cli_broken PRIVATE
  ORDERBOOK_CLI_ENGINE_HEADER="${CMAKE_CURRENT_SOURCE_DIR}/broken_books.hpp"
  ORDERBOOK_CLI_ENGINE=orderbook::broken::BookWithAPriceBug)
target_link_libraries(orderbook_cli_broken PRIVATE orderbook::testkit)
orderbook_set_warnings(orderbook_cli_broken)
# And around one whose events are right and whose depth leaves out a side's third level and beyond: diff sees it only
# at a state checkpoint, so --every must reach the comparison. With a checkpoint after every command it is seen within
# the first few hundred commands, not only at the end of the 2,000.
add_executable(orderbook_cli_broken_depth ${PROJECT_SOURCE_DIR}/tools/orderbook_cli.cpp)
target_compile_definitions(orderbook_cli_broken_depth PRIVATE
  ORDERBOOK_CLI_ENGINE_HEADER="${CMAKE_CURRENT_SOURCE_DIR}/broken_books.hpp"
  ORDERBOOK_CLI_ENGINE=orderbook::broken::BookWithShortDepth)
target_link_libraries(orderbook_cli_broken_depth PRIVATE orderbook::testkit)
orderbook_set_warnings(orderbook_cli_broken_depth)
add_test(NAME cli.broken_depth.diff_compares_states_as_often_as_every_says
  COMMAND orderbook_cli_broken_depth diff 3 2000 --target 50 --every 1)
set_tests_properties(cli.broken_depth.diff_compares_states_as_often_as_every_says PROPERTIES
  PASS_REGULAR_EXPRESSION
  "the engine differs from the reference model: after command [0-9]?[0-9]?[0-9]: (bids|asks) depth")
add_test(NAME cli.broken_engine.diff_reports_the_difference_then_the_shrunk_commands
  COMMAND orderbook_cli_broken diff 12 3000 --target 100)
set_tests_properties(cli.broken_engine.diff_reports_the_difference_then_the_shrunk_commands PROPERTIES
  PASS_REGULAR_EXPRESSION
  "the engine differs from the reference model: command [0-9]+: events: .*# The books disagree on these 3 commands")
add_test(NAME cli.broken_engine.diffs_scenario_fails_the_engine_and_passes_the_reference
  COMMAND sh -c "\"$1\" diff 12 3000 --target 100 > \"$2\" 2>/dev/null; test $? -eq 1 &&
  \"$1\" --book reference check \"$2\" && ! \"$1\" check \"$2\" && ! \"$1\" --book engine check \"$2\"" sh
          $<TARGET_FILE:orderbook_cli_broken> ${CMAKE_CURRENT_BINARY_DIR}/broken_engine_diff.txt)

# And around one that aborts once the shrinking starts: diff must already have said where the books differ.
add_executable(orderbook_cli_crashing ${PROJECT_SOURCE_DIR}/tools/orderbook_cli.cpp)
target_compile_definitions(orderbook_cli_crashing PRIVATE
  ORDERBOOK_CLI_ENGINE_HEADER="${CMAKE_CURRENT_SOURCE_DIR}/broken_books.hpp"
  ORDERBOOK_CLI_ENGINE=orderbook::broken::BookWithAPriceBugThatCrashesWhenReplayed)
target_link_libraries(orderbook_cli_crashing PRIVATE orderbook::testkit)
orderbook_set_warnings(orderbook_cli_crashing)
add_test(NAME cli.crashing_engine.diff_reports_the_difference_before_shrinking
  COMMAND sh -c "\"$1\" diff 12 3000 --target 100 > /dev/null 2> \"$2\"; code=$?;
  test $code -gt 1 && grep -q 'the engine differs from the reference model: command 54: events' \"$2\"" sh
          $<TARGET_FILE:orderbook_cli_crashing> ${CMAKE_CURRENT_BINARY_DIR}/crashing_engine_diff.err)

# run and audit use the book --book names too. The broken engine's transcript of a random stream fails the reference
# model's check, and its --book reference transcript passes; its audit fails, and its --book reference audit passes.
add_test(NAME cli.broken_engine.run_uses_the_book_named
  COMMAND sh -c "\"$1\" gen 12 3000 --target 100 > \"$3.commands\" && \"$2\" run \"$3.commands\" > \"$3\" &&
  ! \"$1\" --book reference check \"$3\" > /dev/null 2>&1 &&
  \"$2\" --book reference run \"$3.commands\" > \"$3.reference\" && \"$1\" --book reference check \"$3.reference\"" sh
          $<TARGET_FILE:orderbook_cli> $<TARGET_FILE:orderbook_cli_broken> ${CMAKE_CURRENT_BINARY_DIR}/broken_run.txt)
add_test(NAME cli.broken_engine.audit_uses_the_book_named
  COMMAND sh -c "\"$1\" audit 12 3000 --target 100 > /dev/null 2>&1; test $? -eq 1 &&
  \"$1\" --book reference audit 12 3000 --target 100 > /dev/null" sh $<TARGET_FILE:orderbook_cli_broken>)
add_test(NAME cli.broken_depth.diff_of_a_state_difference_exits_1
  COMMAND sh -c "\"$1\" diff 3 2000 --target 50 --every 1 > /dev/null 2>&1; test $? -eq 1" sh
          $<TARGET_FILE:orderbook_cli_broken_depth>)

# replay checks the feed against the book it follows. The price bug's trades don't fit the feed's record of the
# makers (a trade a tick away from the maker's price); the short depth is caught at the first snapshot that has a
# third level.
add_test(NAME cli.broken_engine.replay_fails_when_the_events_dont_fit_the_feed
  COMMAND sh -c "\"$1\" gen 12 3000 --target 100 | \"$2\" replay > /dev/null 2> \"$3\"; test $? -eq 1 &&
  grep -q 'the depth feed disagrees with the book' \"$3\"" sh
          $<TARGET_FILE:orderbook_cli> $<TARGET_FILE:orderbook_cli_broken> ${CMAKE_CURRENT_BINARY_DIR}/broken_replay.err)
add_test(NAME cli.broken_depth.replay_fails_when_the_books_depth_differs_from_the_feeds
  COMMAND sh -c "\"$1\" gen 3 2000 --target 50 | \"$2\" replay --every 10 --levels 5 > /dev/null 2> \"$3\";
  test $? -eq 1 && grep -q \"levels are '.*' and the book's\" \"$3\"" sh
          $<TARGET_FILE:orderbook_cli> $<TARGET_FILE:orderbook_cli_broken_depth> ${CMAKE_CURRENT_BINARY_DIR}/short_replay.err)

# The gateway's executable end to end: it serves the load generator over TCP, stops on SIGINT and prints its counts.
# The load generator is a benchmark target, so only where benchmarks are built (not under the sanitizers).
if(ORDERBOOK_BUILD_BENCHMARKS)
  add_test(NAME gateway.the_executable_serves_the_load_generator_and_stops_on_sigint
    COMMAND ${PROJECT_SOURCE_DIR}/scripts/gateway_smoke.sh $<TARGET_FILE:orderbook_gateway_server>
            $<TARGET_FILE:orderbook_loadgen>)
  set_tests_properties(gateway.the_executable_serves_the_load_generator_and_stops_on_sigint PROPERTIES TIMEOUT 120)

  # The figures' script (make histograms) on both tools' --csv files from brief runs: it draws all four figures and
  # parses each back as XML. Python's standard library is all it needs.
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME plot_latency.draws_the_four_figures_from_both_tools_csv_files
      COMMAND sh -c "rm -rf \"$4\" && mkdir -p \"$4\" &&
        \"$1\" 3000 --csv \"$4/latency.csv\" >/dev/null && \"$2\" --commands 2000 --csv \"$4/loadgen.csv\" >/dev/null &&
        \"$3\" \"$5\" \"$4/latency.csv\" \"$4/loadgen.csv\" \"$4/figures\" --note test &&
        test $(ls \"$4/figures\" | wc -l) -eq 4" sh
              $<TARGET_FILE:orderbook_latency> $<TARGET_FILE:orderbook_loadgen> ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_BINARY_DIR}/plot_latency ${PROJECT_SOURCE_DIR}/scripts/plot_latency.py)
    set_tests_properties(plot_latency.draws_the_four_figures_from_both_tools_csv_files PROPERTIES TIMEOUT 120)
  endif()
endif()

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
cli_exit_test(cli.exit.version_is_0 0 version)
cli_exit_test(cli.exit.version_with_arguments_is_2 2 version now)
cli_exit_test(cli.exit.options_out_of_range_are_2 2 gen 1 10 --range 600000000)
cli_exit_test(cli.exit.no_arguments_are_2 2)
cli_exit_test(cli.exit.a_passing_audit_is_0 0 audit 5 2000 --target 50)
cli_exit_test(cli.exit.a_diff_with_no_difference_is_0 0 diff 5 2000 --target 50)
cli_exit_test(cli.exit.an_unknown_book_is_2 2 --book fast check ${CMAKE_CURRENT_SOURCE_DIR}/scenarios/01_resting.txt)
cli_exit_test(cli.exit.a_book_without_a_name_is_2 2 --book)
cli_exit_test(cli.exit.a_book_named_for_diff_is_2 2 --book engine diff 5 2000)
cli_exit_test(cli.exit.every_outside_diff_is_2 2 gen 5 10 --every 5)
cli_exit_test(cli.exit.every_of_0_is_2 2 diff 5 10 --every 0)
cli_exit_test(cli.exit.a_replay_is_0 0 replay ${CMAKE_CURRENT_SOURCE_DIR}/cli/replay.txt)
cli_exit_test(cli.exit.replay_every_0_is_2 2 replay ${CMAKE_CURRENT_SOURCE_DIR}/cli/replay.txt --every 0)
cli_exit_test(cli.exit.replay_levels_without_a_value_is_2 2 replay --levels)
cli_exit_test(cli.exit.replay_of_two_files_is_2 2 replay ${CMAKE_CURRENT_SOURCE_DIR}/cli/replay.txt
              ${CMAKE_CURRENT_SOURCE_DIR}/cli/replay.txt)
cli_exit_test(cli.exit.replay_of_a_missing_file_is_1 1 replay ${CMAKE_CURRENT_BINARY_DIR}/no_such_file.txt)
cli_exit_test(cli.exit.replay_of_a_bad_command_is_1 1 replay ${CMAKE_CURRENT_SOURCE_DIR}/cli/bad_command.txt)
