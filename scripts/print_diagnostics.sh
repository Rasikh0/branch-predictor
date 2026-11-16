#!/bin/bash
# Print the internal counts from a workload report: RAS pushes, pops,
# overflows and underflows, indirect predictor lookups and hits, and, for the
# unified designs (uni_*), the table's contents at the end of the run and the
# number of second (dependent) probes.
#
# Usage:  scripts/print_diagnostics.sh <workload> <predictor>
#   e.g.  scripts/print_diagnostics.sh gzip uni_hist
ROOT=$(cd "$(dirname "$0")/.." && pwd)
W=$1; P=${2:-baseline}
F=$ROOT/results/workloads/$W.$P.txt
if [ -z "$W" ] || [ ! -f "$F" ]; then
  echo "usage: $0 <workload> <predictor>   (no report at $F)" >&2
  exit 1
fi
echo "== $W, $P"
sed -n '/^  TAGE provider\|^  RAS pushes/,/^Top /p' "$F" | grep -v "^Top " | sed '/^$/d'
