#!/bin/bash
# Section 9 bonus: run the branch-pattern test (tests/t_patterns.c) under
# each direction predictor and print, for every pattern branch, how often
# each predictor got it wrong.
#
# Usage:  scripts/run_patterns.sh
# Writes results/patterns/t_patterns.<predictor>.txt (full reports, with the
# per-branch profile and the TAGE component counts) and prints the table.
set -e
if [ -z "$PIN_ROOT" ]; then
  echo "PIN_ROOT is not set; see README.md" >&2
  exit 1
fi
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TOOL=$ROOT/pintool/obj-intel64/bpred.so
OUT=$ROOT/results/patterns
PREDS=(final_tour final tage_base tage_l tage)

make -s -C "$ROOT/tests" t_patterns
mkdir -p "$OUT"
cd "$ROOT/tests"
for p in "${PREDS[@]}"; do
  printf "Running t_patterns with %-10s ... " "$p"
  env -i PATH=/usr/bin:/bin setarch "$(uname -m)" -R "$PIN_ROOT/pin" -t "$TOOL" \
      -predictor "$p" -top 40 -o "$OUT/t_patterns.$p.txt" -- ./t_patterns \
      > "$OUT/t_patterns.out" 2> "$OUT/t_patterns.$p.err" || { echo FAILED; tail "$OUT/t_patterns.$p.err"; exit 1; }
  echo done
done
echo
python3 "$ROOT/scripts/patterns_table.py" "${PREDS[@]}"
