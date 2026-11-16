#!/bin/bash
# Run every workload under the Pin tool with one predictor configuration.
#
# Usage:  scripts/run_workloads.sh [predictor] [extra tool options]
#         (predictors: baseline gshare12 gshare itp itp4 itp4_gh ras64 btb4way
#          final_v1 final uni_hist uni_phr uni_2probe final_tour tage_small tage_base
#          tage_l tage)
#   e.g.  scripts/run_workloads.sh
#         scripts/run_workloads.sh final -max_insts 50000000
#
# For each workload, writes to results/workloads/:
#   <workload>.<predictor>.txt   the tool's report, ending with the 20 branches
#                                with the most wrong next PCs
#   <workload>.out               the program's own output
# then prints the summary tables (scripts/collect_results.py).
#
# To make instruction counts identical on any machine and account:
#   - programs run with a fixed, minimal environment (env -i), since the
#     dynamic loader and libc process every environment variable;
#   - programs run from workloads/ with relative paths, so the location of
#     this directory doesn't change their arguments;
#   - program output always goes to a file, never the terminal, since libc
#     runs different code for each.
set -e

if [ -z "$PIN_ROOT" ]; then
  echo "PIN_ROOT is not set; see README.md" >&2
  exit 1
fi

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TOOL=$ROOT/pintool/obj-intel64/bpred.so
W=$ROOT/workloads
OUT=$ROOT/results/workloads
PRED=${1:-baseline}
shift || true
TOOL_OPTS=("$@")

make -s -C "$W"
mkdir -p "$OUT"
cd "$W"

run() {
  local name=$1
  shift
  local start=$SECONDS
  printf "Running %-8s ... " "$name"
  if env -i PATH=/usr/bin:/bin \
       setarch "$(uname -m)" -R "$PIN_ROOT/pin" -t "$TOOL" -predictor "$PRED" -top 20 \
       -o "$OUT/$name.$PRED.txt" "${TOOL_OPTS[@]}" -- "$@" \
       > "$OUT/$name.out" 2> "$OUT/$name.err"; then
    echo "done in $((SECONDS - start)) s"
  else
    echo "FAILED; error output:"
    tail -20 "$OUT/$name.err"
    exit 1
  fi
}

run loop    ./loop
run sort    ./sort
run recurse ./recurse
run interp  ./interp
run gzip    ./gzip-1.12/gzip -c -6 gzip_input.txt

# Set NO_TABLES=1 to skip the tables (scripts/run_all.sh does).
if [ -z "$NO_TABLES" ]; then
  echo
  python3 "$ROOT/scripts/collect_results.py" "$PRED"
fi
