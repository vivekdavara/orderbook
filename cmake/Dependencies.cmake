# Test and benchmark dependencies, fetched at configure time as release tarballs pinned by SHA-256, so every build
# compiles exactly the same sources. The library itself depends only on the standard library. SYSTEM makes their
# headers system headers for our targets, so a warning inside them can't fail a -Werror build.
include(FetchContent)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.tar.gz
  URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  SYSTEM)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
FetchContent_Declare(benchmark
  URL https://github.com/google/benchmark/archive/refs/tags/v1.9.5.tar.gz
  URL_HASH SHA256=9631341c82bac4a288bef951f8b26b41f69021794184ece969f8473977eaa340
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  SYSTEM)

if(ORDERBOOK_BUILD_TESTS)
  FetchContent_MakeAvailable(googletest)
endif()
if(ORDERBOOK_BUILD_BENCHMARKS)
  FetchContent_MakeAvailable(benchmark)
endif()
