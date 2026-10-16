# Warnings for the project's own targets only; the fetched dependencies keep their own flags.
function(orderbook_set_warnings target)
  set(warnings
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wold-style-cast
    -Wcast-align
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wdouble-promotion
    -Wimplicit-fallthrough)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    list(APPEND warnings -Wduplicated-cond -Wlogical-op -Wuseless-cast)
  else()
    # Clang only. GCC's version works from the optimiser's view of the code, so it fires in optimised builds only,
    # and twice here falsely: GCC 14 inside libstdc++'s streambuf (CI at 6c276be), and GCC 15 in Price's defaulted
    # comparison inlined into a test (the day-1 review, on this Mac).
    list(APPEND warnings -Wnull-dereference)
  endif()
  if(ORDERBOOK_WERROR)
    list(APPEND warnings -Werror)
  endif()
  target_compile_options(${target} PRIVATE ${warnings})
endfunction()
