#!/usr/bin/env bash
# Benchmark the web server's GET /ping endpoint with wrk
#
# Usage: ./bench.sh [duration] [threads] [connections]

set -euo pipefail

DURATION="${1:-15s}"
THREADS="${2:-4}"
CONNS="${3:-128}"

export WORKERS="${WORKERS:-6}"
PORT=8080
URL="http://127.0.0.1:${PORT}/ping"

# Check for the binary
if [[ ! -x build/server ]]; then
  echo "build/server not found -- run: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build"
  exit 1
fi

# Start the server, redirecting its stdout away to hide logs
./build/server >/dev/null 2>&1 &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null || true' EXIT

# Wait for the port to accept connections
for _ in $(seq 1 50); do
  if exec 3<>"/dev/tcp/127.0.0.1/${PORT}" 2>/dev/null; then exec 3>&- 3<&-; break; fi
  sleep 0.1
done

# Warm-up run (results discarded)
wrk -t"$THREADS" -c"$CONNS" -d3s --latency "$URL" >/dev/null 2>&1 || true

# Actual run
echo "== wrk -t$THREADS -c$CONNS -d$DURATION  $URL  (server WORKERS=$WORKERS) =="
wrk -t"$THREADS" -c"$CONNS" -d"$DURATION" --latency "$URL"
