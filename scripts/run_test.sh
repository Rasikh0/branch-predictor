#!/bin/bash
# Run one directed test under the Pin tool and save everything needed to
# check it by hand.
#
# Usage:  scripts/run_test.sh <test-name> [extra tool options]
#   e.g.  scripts/run_test.sh t_cond
#
# Writes to results/validation/:
#   <test>.csv           event log (main executable only, first 1000 events)
#   <test>.summary.txt   end-of-run summary
#   <test>.objdump.txt   disassembly to compare addresses and lengths against
set -e

if [ -z "$PIN_ROOT" ]; then
  echo "PIN_ROOT is not set; see README.md" >&2
  exit 1
fi
if [ $# -lt 1 ]; then
  echo "usage: $0 <test-name> [extra tool options]" >&2
  exit 1
fi

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TOOL=$ROOT/pintool/obj-intel64/bpred.so
TEST=$1; shift
BIN=$ROOT/tests/$TEST
OUT=$ROOT/results/validation
mkdir -p "$OUT"

# setarch -R turns off address randomization so library addresses repeat
# from run to run (the test programs themselves are built at fixed addresses).
setarch "$(uname -m)" -R "$PIN_ROOT/pin" -t "$TOOL" \
    -log "$OUT/$TEST.csv" -o "$OUT/$TEST.summary.txt" "$@" -- "$BIN"

objdump -d --no-show-raw-insn "$BIN" > "$OUT/$TEST.objdump.txt"
echo "Wrote $OUT/$TEST.csv, $TEST.summary.txt, $TEST.objdump.txt"
