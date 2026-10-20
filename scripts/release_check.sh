#!/bin/sh
# The release check: every check this project has, on the working tree and on a clean export of HEAD.
#
#   scripts/release_check.sh          make check; make check on a clean `git archive` of HEAD (so nothing untracked
#                                     or ignored helps the build); the export's `orderbook version` against
#                                     CMakeLists.txt's project() version and, if HEAD has a release tag (vX.Y.Z),
#                                     against the tag; make asan, make tsan, make gcc-check, make soak
#   scripts/release_check.sh quick    the first three only
#
# Prints each step's result and time, and exits 1 if any step failed. The measurements (make bench, make latency,
# make loadgen, make histograms) aren't run: they need a quiet machine, and their numbers aren't pass or fail.
set -u
cd "$(dirname "$0")/.." || exit 1
mode=${1:-full}
case $mode in
  full | quick) ;;
  *)
    echo "usage: scripts/release_check.sh [quick]" >&2
    exit 2
    ;;
esac

export_dir=$(mktemp -d "${TMPDIR:-/tmp}/orderbook-release.XXXXXX") || exit 1
log_dir="$export_dir/logs"
mkdir -p "$log_dir"
failed=0
cleanup() {
  rm -rf "$export_dir"
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM

# step NAME COMMAND...: runs the command with its output in a log, then prints ok or FAILED and the time it took.
step() {
  name=$1
  shift
  start=$(date +%s)
  if "$@" > "$log_dir/$name.log" 2>&1; then
    echo "ok      $name ($(($(date +%s) - start)) s)"
  else
    code=$?
    echo "FAILED  $name (exit $code, $(($(date +%s) - start)) s); the end of its log:"
    tail -20 "$log_dir/$name.log" | sed 's/^/        /'
    failed=1
  fi
}

# The version checks, on the export's build.
versions() {
  project=$(sed -n 's/^  VERSION \([0-9][0-9.]*\)$/\1/p' CMakeLists.txt)
  printed=$("$export_dir/src/build/dev/orderbook" version | sed -n '1s/^orderbook //p')
  echo "project() says $project; orderbook version says $printed"
  [ -n "$project" ] && [ "$project" = "$printed" ] || return 1
  if tag=$(git describe --exact-match --tags --match 'v[0-9]*' HEAD 2>/dev/null); then
    echo "HEAD is tagged $tag"
    [ "$tag" = "v$printed" ] || return 1
  else
    echo "HEAD has no release tag"
  fi
}

echo "release check ($mode) of $(git rev-parse --short HEAD), $(date '+%Y-%m-%d %H:%M:%S')"
if [ -n "$(git status --porcelain)" ]; then
  echo "note: the working tree has changes; the clean export is HEAD without them"
fi
mkdir -p "$export_dir/src"
step export sh -c 'git archive HEAD | tar -x -C "$1"' sh "$export_dir/src"
step check make check
step clean-export-check make -C "$export_dir/src" check
step version versions
if [ "$mode" = full ]; then
  step asan make asan
  step tsan make tsan
  step gcc-check make gcc-check
  step soak make soak
fi
if [ "$failed" -ne 0 ]; then
  echo "release check: FAILED"
  exit 1
fi
echo "release check: every step passed"
