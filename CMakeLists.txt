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
option(ORDERBOOK_WERROR "Treat compiler warnings as errors" OFF)
set(ORDERBOOK_SANITIZE "" CACHE STRING "Sanitizers to build with, e.g. address,undefined (empty for none)")

include(cmake/Warnings.cmake)
include(cmake/Sanitizers.cmake)

add_library(orderbook
  src/text.cpp
  src/types.cpp)
add_library(orderbook::orderbook ALIAS orderbook)
target_include_directories(orderbook PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(orderbook PUBLIC cxx_std_20)
orderbook_set_warnings(orderbook)

# What tests, benchmarks and tools use to check a book: the reference model (the oracle) and, as they are added, the
# scenario runner, the random command generator and the invariant checks. Kept out of the engine's library.
add_library(orderbook_testkit
  src/random_flow.cpp
  src/reference_book.cpp
  src/scenario.cpp)
add_library(orderbook::testkit ALIAS orderbook_testkit)
target_link_libraries(orderbook_testkit PUBLIC orderbook::orderbook)
orderbook_set_warnings(orderbook_testkit)

if(ORDERBOOK_BUILD_TESTS)
  include(cmake/Dependencies.cmake)
  enable_testing()
  add_subdirectory(tests)
endif()
