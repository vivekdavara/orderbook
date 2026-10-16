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
    -Wnull-dereference
    -Wdouble-promotion
    -Wimplicit-fallthrough)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    list(APPEND warnings -Wduplicated-cond -Wlogical-op -Wuseless-cast)
  endif()
  if(ORDERBOOK_WERROR)
    list(APPEND warnings -Werror)
  endif()
  target_compile_options(${target} PRIVATE ${warnings})
endfunction()
