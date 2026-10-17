#!/bin/sh
# The differential soak: `orderbook diff` (the engine against the reference model, every event compared) on six mixes
# of random commands, four seeds each, 1.04 billion commands in all, four runs at a time. Prints one line per run: ok,
# DIFFERENT (diff found a difference and exited 1), or FAILED with any other exit code (a crash, a usage error).
# Exits 1 if any run wasn't ok, and prints each such run's report and shrunk scenario file at the end.
#
#   scripts/diff_soak.sh [ORDERBOOK_BINARY]      (a relative path is from where it is run; without one, the
#                                                 release build's; make soak builds that one first)
#
# Ctrl-C (or SIGTERM) stops the runs in flight and removes the work directory.
set -u
bin=""
if [ $# -ge 1 ]; then
  case $1 in
    /*) bin=$1 ;;
    *) bin=$PWD/$1 ;;  # resolved before the cd below
  esac
fi
cd "$(dirname "$0")/.."
[ -n "$bin" ] || bin=$PWD/build/release/orderbook
if [ ! -f "$bin" ] || [ ! -x "$bin" ]; then
  echo "diff_soak: no binary at $bin (make soak builds it)" >&2
  exit 2
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/diff_soak.XXXXXX")

# Runs started with & ignore SIGINT in a non-interactive shell, so an interrupt has to stop them itself (each run
# writes its process id to a file); and dash, /bin/sh on Ubuntu, runs no EXIT trap when a signal ends the shell, so the
# signal traps clean up and exit themselves.
stop() {
  for pid_file in "$work"/*.pid; do
    if [ -f "$pid_file" ]; then kill "$(cat "$pid_file")" 2>/dev/null; fi
  done
  wait
  rm -rf "$work"
}
trap 'rm -rf "$work"' EXIT
trap 'trap - EXIT; stop; exit 130' INT
trap 'trap - EXIT; stop; exit 143' TERM

# One run: its seed is its name. Writes "<status> <seconds>s diff <args>" and keeps its report and scenario.
run() {
  start=$(date +%s)
  "$bin" diff "$@" >"$work/$1.out" 2>"$work/$1.err" &
  echo $! >"$work/$1.pid"
  wait $!
  code=$?
  case $code in
    0) status=ok ;;
    1) status=DIFFERENT ;;
    *) status="FAILED (exit $code)" ;;
  esac
  echo "$status $(($(date +%s) - start))s diff $*" >"$work/$1.line"
}
wave() {  # four seeds of one mix, at once: wave FIRST_SEED COUNT OPTIONS...
  first=$1
  shift
  for seed in $first $((first + 1)) $((first + 2)) $((first + 3)); do run "$seed" "$@" & done
  wait
  for seed in $first $((first + 1)) $((first + 2)) $((first + 3)); do cat "$work/$seed.line"; done
}

started=$(date +%s)
wave 1 50000000 --target 500 --every 10000                                 # the default mix
wave 5 50000000 --target 200 --range 3 --max-quantity 20 --every 1000      # long queues at three prices
wave 9 50000000 --target 50 --range 5 --max-quantity 1 --every 1000        # one-lot orders: levels come and go
wave 13 50000000 --target 5 --max-quantity 1000000000 --every 100          # large quantities, a nearly empty book
wave 17 50000000 --target 100 --invalid 300 --every 1000                   # 30% spoiled orders and their resends
wave 21 10000000 --target 5000 --range 2000 --every 100000                 # a deep book of many levels
echo "24 runs, 1040000000 commands, in $(($(date +%s) - started)) s"

status=0
for seed in $(seq 1 24); do
  if ! grep -q '^ok ' "$work/$seed.line"; then
    status=1
    echo "== seed $seed:"
    cat "$work/$seed.err" "$work/$seed.out"
  fi
done
exit $status
