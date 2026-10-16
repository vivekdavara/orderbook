# Review 2: tools/orderbook_cli.cpp:9-10 (and README, "run's transcript is itself a scenario file") promise that
# `orderbook gen ... | orderbook run` prints a scenario file that `orderbook check` passes. With no commands (COUNT 0,
# or an input of only comments and blank lines) run prints just "= bids" and "= asks", and check rejects that:
# "line 1: an expectation before any command ('> ...')".
add_test(NAME review2.cli.an_empty_transcript_is_a_passing_scenario
  COMMAND sh -c "\"$1\" gen 1 0 | \"$1\" run > \"$2\" && \"$1\" check \"$2\"" sh
          $<TARGET_FILE:orderbook_cli> ${CMAKE_CURRENT_BINARY_DIR}/review2_empty_transcript.txt)
