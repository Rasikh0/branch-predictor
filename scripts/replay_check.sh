#!/bin/bash
# Check that a recorded event log reproduces the Pin run exactly.
#
# Usage:  scripts/replay_check.sh <test-name>
#   e.g.  scripts/replay_check.sh t_calls
#
# 1. Runs the test under Pin, logging every event from the start
#    (loader and libraries included) to results/validation/<test>.full.csv.
# 2. Replays that log through the same predictor without Pin
#    (tests/unit/replay) and compares every prediction with the logged one.
# Success ends with "Predictions that differ from the log: 0".
set -e

if [ -z "$PIN_ROOT" ]; then
  echo "PIN_ROOT is not set; see README.md" >&2
  exit 1
fi
if [ $# -ne 1 ]; then
  echo "usage: $0 <test-name>" >&2
  exit 1
fi

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TEST=$1
OUT=$ROOT/results/validation
mkdir -p "$OUT"

make -s -C "$ROOT/tests/unit" replay

setarch "$(uname -m)" -R "$PIN_ROOT/pin" -t "$ROOT/pintool/obj-intel64/bpred.so" \
    -log "$OUT/$TEST.full.csv" -log_main 0 -log_count 100000000 \
    -o "$OUT/$TEST.full.summary.txt" -- "$ROOT/tests/$TEST" > /dev/null 2>&1

echo "Pin run finished; replaying $OUT/$TEST.full.csv"
"$ROOT/tests/unit/replay" "$OUT/$TEST.full.csv"
