#!/bin/bash
# Build + run with the JSON event log also tee'd to /tmp for easy sharing.
# Pretty-print live JSONL:  tail -f /tmp/thorn-debug.log | jq -c .
# Filter to pure JSON:      grep '^{' /tmp/thorn-debug.log | jq -c .
set -euo pipefail
cd "$(dirname "$0")"
make all
./build/thorn --debug "$@" 2>&1 | tee /tmp/thorn-debug.log
