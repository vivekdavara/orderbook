#!/bin/sh
# Compiles this project's own sources with GCC, in Debug and Release with warnings as errors, to catch GCC-only
# warnings before CI's GCC job does. CI builds with GCC 14 on Linux; this Mac has Homebrew's g++-15. The tests and
# benchmarks don't link here: GoogleTest and Google Benchmark don't compile with GCC against the macOS SDK, so failures
# in them (_deps/) are ignored (the CLI and the fuzz targets, which don't need them, do link), and the script fails only
# if a file of this project fails to compile.
#
#   scripts/gcc_check.sh            (or: make gcc-check)
#   GXX=g++-14 scripts/gcc_check.sh
set -u
cd "$(dirname "$0")/.."
GXX=${GXX:-$(command -v g++-15 || command -v g++-14 || true)}
if [ -z "$GXX" ]; then
  echo "gcc_check: no g++-15 or g++-14 found (set GXX)" >&2
  exit 2
fi
# Reuse the sources the dev build fetched, if it has, rather than downloading them again.
deps=build/dev/_deps
fetch=""
if [ -d "$deps/googletest-src" ] && [ -d "$deps/benchmark-src" ]; then
  fetch="-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=$PWD/$deps/googletest-src -DFETCHCONTENT_SOURCE_DIR_BENCHMARK=$PWD/$deps/benchmark-src"
fi
status=0
for type in Debug Release; do
  dir=build/gcc-$type
  # shellcheck disable=SC2086
  cmake -S . -B "$dir" -G Ninja -DCMAKE_CXX_COMPILER="$GXX" -DCMAKE_BUILD_TYPE=$type -DORDERBOOK_WERROR=ON $fetch \
    >/dev/null || exit 2
  log=$(ninja -C "$dir" -k 0 2>&1)
  ours=$(printf '%s\n' "$log" | grep '^FAILED' | grep -v '_deps/' || true)
  if [ -n "$ours" ]; then
    echo "== $type: these failed with $GXX:"
    printf '%s\n' "$ours"
    printf '%s\n' "$log" | grep -E '(error|warning):' | grep -v -e '_deps/' -e '/SDKs/' | sort -u
    status=1
  else
    echo "== $type: every file of this project compiled with $GXX"
  fi
done
exit $status
