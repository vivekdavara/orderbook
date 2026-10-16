# Test dependencies, fetched at configure time as release tarballs pinned by SHA-256, so every build
# compiles exactly the same sources. The library itself depends only on the standard library.
include(FetchContent)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.tar.gz
  URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
  DOWNLOAD_EXTRACT_TIMESTAMP ON)

if(ORDERBOOK_BUILD_TESTS)
  FetchContent_MakeAvailable(googletest)
endif()
