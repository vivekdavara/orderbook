# Two fuzz targets. With ORDERBOOK_FUZZ (Clang with libFuzzer, as in CI's fuzz job) they are libFuzzer binaries;
# otherwise each is built with standalone_main.cpp, and CTest runs it on the scenario files and random inputs.
foreach(target text_fuzz book_fuzz)
  if(ORDERBOOK_FUZZ)
    add_executable(${target} ${target}.cpp)
    target_link_options(${target} PRIVATE -fsanitize=fuzzer)
  else()
    add_executable(${target} ${target}.cpp standalone_main.cpp)
  endif()
  target_link_libraries(${target} PRIVATE orderbook::testkit)
  orderbook_set_warnings(${target})
endforeach()

if(NOT ORDERBOOK_FUZZ AND ORDERBOOK_BUILD_TESTS)
  file(GLOB fuzz_seed_files CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/scenarios/*.txt)
  add_test(NAME fuzz.text_on_the_scenario_files COMMAND text_fuzz ${fuzz_seed_files})
  # Each line on its own too: a command line, a depth line's levels, an event line. Whole files never parse as one
  # command, so without this the round-trip checks would run only in CI's libFuzzer job.
  add_test(NAME fuzz.text_on_each_scenario_line COMMAND text_fuzz --lines ${fuzz_seed_files})
  add_test(NAME fuzz.text_on_random_bytes COMMAND text_fuzz --random 2000 1)
  add_test(NAME fuzz.book_on_random_bytes COMMAND book_fuzz --random 2000 2)
endif()
