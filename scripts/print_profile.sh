#!/bin/bash
# Print the per-branch profile from a workload report: the static branches
# with the most wrong next PCs, with their function names.
#
# Usage:  scripts/print_profile.sh <workload> [predictor] [rows]
#   e.g.  scripts/print_profile.sh gzip baseline 10
ROOT=$(cd "$(dirname "$0")/.." && pwd)
W=$1; P=${2:-baseline}; N=${3:-20}
F=$ROOT/results/workloads/$W.$P.txt
if [ -z "$W" ] || [ ! -f "$F" ]; then
  echo "usage: $0 <workload> [predictor] [rows]   (no report at $F)" >&2
  exit 1
fi
echo "== $W, $P"
sed -n '/^Top /,$p' "$F" | head -n $((N + 5))
