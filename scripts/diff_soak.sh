#!/bin/sh
# The differential soak: `orderbook diff` (the engine against the reference model, every event compared) on six mixes
# of random commands, four seeds each, 1.04 billion commands in all, four runs at a time. Prints one line per run and
# exits 1 if any run found a difference; that run's shrunk scenario file is printed at the end.
#
#   scripts/diff_soak.sh [ORDERBOOK_BINARY]      (or: make soak, which builds the release binary first)
set -u
cd "$(dirname "$0")/.."
bin=${1:-build/release/orderbook}
if [ ! -x "$bin" ]; then
  echo "diff_soak: no binary at $bin (make soak builds it)" >&2
  exit 2
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/diff_soak.XXXXXX")
trap 'rm -rf "$work"' EXIT

# One run: its seed is its name. Writes "ok|DIFFERENT <seconds>s diff <args>" and keeps a difference's scenario.
run() {
  start=$(date +%s)
  if "$bin" diff "$@" >"$work/$1.out" 2>"$work/$1.err"; then status=ok; else status=DIFFERENT; fi
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
  if grep -q '^DIFFERENT' "$work/$seed.line"; then
    status=1
    echo "== seed $seed:"
    cat "$work/$seed.err" "$work/$seed.out"
  fi
done
exit $status
