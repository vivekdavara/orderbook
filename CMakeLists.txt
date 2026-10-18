cmake_minimum_required(VERSION 3.25)

project(orderbook
  VERSION 0.1.0
  DESCRIPTION "Limit order book and matching engine"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ORDERBOOK_BUILD_TESTS "Build the GoogleTest suite" ${PROJECT_IS_TOP_LEVEL})
option(ORDERBOOK_BUILD_BENCHMARKS "Build the Google Benchmark suite" ${PROJECT_IS_TOP_LEVEL})
option(ORDERBOOK_WERROR "Treat compiler warnings as errors" OFF)
set(ORDERBOOK_SANITIZE "" CACHE STRING "Sanitizers to build with, e.g. address,undefined (empty for none)")
option(ORDERBOOK_COVERAGE "Build with Clang's source-based coverage (make coverage)" OFF)
option(ORDERBOOK_FUZZ "Build the fuzz targets for libFuzzer (Clang only)" OFF)

find_package(Threads REQUIRED)

include(cmake/Warnings.cmake)
include(cmake/Sanitizers.cmake)
if(ORDERBOOK_COVERAGE)
  add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
  add_link_options(-fprofile-instr-generate)
endif()
if(ORDERBOOK_FUZZ)
  # Coverage instrumentation for libFuzzer in everything; only the fuzz targets link its main().
  add_compile_options(-fsanitize=fuzzer-no-link)
endif()

# The engine: the book itself, its thread and rings, its vocabulary types and their text form.
add_library(orderbook
  src/book.cpp
  src/engine_thread.cpp
  src/text.cpp
  src/types.cpp)
add_library(orderbook::orderbook ALIAS orderbook)
target_include_directories(orderbook PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(orderbook PUBLIC cxx_std_20)
target_link_libraries(orderbook PUBLIC Threads::Threads)
orderbook_set_warnings(orderbook)

# What tests, benchmarks and tools use to check a book: the reference model (the oracle), day 2's engine (the
# baseline day 3 is measured against), the scenario runner, the random command generator, the invariant checks, the
# minimizer and the differential harness. Kept out of the engine's library.
add_library(orderbook_testkit
  src/check.cpp
  src/differential.cpp
  src/map_book.cpp
  src/minimize.cpp
  src/random_flow.cpp
  src/reference_book.cpp
  src/scenario.cpp)
add_library(orderbook::testkit ALIAS orderbook_testkit)
target_link_libraries(orderbook_testkit PUBLIC orderbook::orderbook)
orderbook_set_warnings(orderbook_testkit)

# The command-line tool over the test kit (tools/orderbook_cli.cpp), installed as `orderbook`.
add_executable(orderbook_cli tools/orderbook_cli.cpp)
set_target_properties(orderbook_cli PROPERTIES OUTPUT_NAME orderbook)
target_link_libraries(orderbook_cli PRIVATE orderbook::testkit)
orderbook_set_warnings(orderbook_cli)

if(ORDERBOOK_BUILD_TESTS OR ORDERBOOK_BUILD_BENCHMARKS)
  include(cmake/Dependencies.cmake)
endif()

if(ORDERBOOK_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(ORDERBOOK_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()

add_subdirectory(fuzz)
