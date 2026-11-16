#!/bin/bash
# Run every workload with every predictor configuration, then print the
# comparison tables (spec Sections 5 and 7).
#
# Usage:  scripts/run_all.sh [predictor ...]
#   (default: all configurations: baseline gshare12 gshare itp itp4 itp4_gh
#    ras64 btb4way final_v1 final uni_hist uni_phr uni_2probe final_tour tage_small
#    tage_base tage_l tage)
#   e.g.  scripts/run_all.sh baseline final uni_hist uni_phr uni_2probe
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PREDS=("$@")
[ ${#PREDS[@]} -eq 0 ] && PREDS=(baseline gshare12 gshare itp itp4 itp4_gh ras64 btb4way final_v1 final
                                 uni_hist uni_phr uni_2probe final_tour tage_small tage_base tage_l tage)

for p in "${PREDS[@]}"; do
  echo "=== $p"
  NO_TABLES=1 "$ROOT/scripts/run_workloads.sh" "$p"
done
echo
python3 "$ROOT/scripts/collect_results.py" baseline | sed -n '/Table 1/,/^$/p'
python3 "$ROOT/scripts/compare_predictors.py" "${PREDS[@]}"
