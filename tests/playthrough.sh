#!/bin/bash
# Headless regression check: graph validity + a full four-area playthrough.
# Run via `make test`. Exits non-zero on any failure (CI-friendly).
set -euo pipefail
cd "$(dirname "$0")/.."

make all >/dev/null
BIN=./build/thorn
LOG=thorn-debug.log
fail(){ echo "FAIL: $1"; exit 1; }

echo "[1/4] selftest (room graph)"
$BIN --selftest 2>&1 | tail -1 | grep -q "0 errors" || fail "selftest reported errors"

echo "[2/4] headless playthrough (16000 frames)"
$BIN --headless --frames 16000 >/dev/null 2>&1

echo "[3/4] all four areas reached"
for a in "Sunken Mines" "The Mire" "The Ashlands" "Usurper"; do
    grep -q "\"area\":\"[^\"]*$a" "$LOG" || fail "area not reached: $a"
done

echo "[4/4] systems fired (shard gate, lever-bridge, transitions)"
grep -q '"ev":"door","id":[0-9]*,"gateOpen"' "$LOG" || fail "shard gate never opened"
grep -q '"ev":"lever"' "$LOG"      || fail "no lever/bridge event"
grep -q '"ev":"transition"' "$LOG" || fail "no room transitions"
grep -q '"ev":"pickup"' "$LOG"     || fail "no pickups"

echo "PASS: graph valid, four areas reached, core systems fired"
