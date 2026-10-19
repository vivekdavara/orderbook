#pragma once

// Whether a failed socket call failed only because it would have blocked (EAGAIN, or EWOULDBLOCK). POSIX lets the
// two differ; on Linux and macOS they are the same number, and there GCC's -Wlogical-op flags `e == EAGAIN || e ==
// EWOULDBLOCK` as a comparison made twice (with glibc's errno, a const function call, at least: CI's GCC 14 job).

#include <cerrno>

namespace orderbook {

inline bool would_block(int error) {
#if EAGAIN == EWOULDBLOCK
  return error == EAGAIN;
#else
  return error == EAGAIN || error == EWOULDBLOCK;
#endif
}

}  // namespace orderbook
