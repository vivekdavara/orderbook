#!/bin/sh
# Starts orderbook_gateway on a free port, drives it with the load generator (one saturated row, 4 connections),
# stops it with SIGINT, and checks that it stopped cleanly and counted the 4 sessions. CTest runs it
# (gateway.the_executable_serves_the_load_generator_and_stops_on_sigint).
#
#   scripts/gateway_smoke.sh GATEWAY LOADGEN
set -u
gateway=$1
loadgen=$2
out=$(mktemp)
pid=""
trap '[ -n "$pid" ] && kill "$pid" 2>/dev/null; rm -f "$out"' EXIT
"$gateway" --port 0 > "$out" &
pid=$!
for _ in $(seq 1 200); do
  grep -q listening "$out" && break
  sleep 0.05
done
port=$(sed -n 's/.*listening on 127\.0\.0\.1:\([0-9][0-9]*\).*/\1/p' "$out")
if [ -z "$port" ]; then
  echo "gateway_smoke: the gateway didn't say which port it listens on" >&2
  cat "$out" >&2
  exit 1
fi
"$loadgen" --port "$port" --commands 2000 --rate 0 || exit 1
kill -INT "$pid"
wait "$pid"
code=$?
pid=""
cat "$out"
if [ "$code" -ne 0 ]; then
  echo "gateway_smoke: the gateway exited with $code" >&2
  exit 1
fi
grep -q "stopped; 4 sessions" "$out"
