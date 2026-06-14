#!/bin/bash
# Thorn - build and run. Incremental: `make` only recompiles when src or the
# raylib archive changed, so an unchanged tree launches instantly.
set -euo pipefail
cd "$(dirname "$0")"

if [[ ! -f vendor/raylib/lib/libraylib.a ]]; then
  echo "Building vendored raylib 6.0 (one-time, a few minutes)..."
  make raylib6
fi

make all

# Always run with --debug. The game writes a newline-delimited JSON event stream
# to ./thorn-debug.log (a recurring ~5 Hz state snapshot plus discrete events).
# Tail it with ./debug.sh or: tail -f thorn-debug.log | jq -c .
# Extra args pass straight through (e.g. ./run.sh --no-enemies --rate 1).
./build/thorn --debug "$@"
echo "JSON event log: $(pwd)/thorn-debug.log"
