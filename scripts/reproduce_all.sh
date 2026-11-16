#!/bin/bash
# Rebuild and rerun every result in the report, from scratch (about 5-10 minutes).
#
# Usage:  scripts/reproduce_all.sh
# Needs PIN_ROOT set and internet access the first time (to download gzip 1.12).
# Regenerates everything under results/: directed-test logs, the replay check,
# every workload report and comparison.csv, and the pattern test.
set -e
if [ -z "$PIN_ROOT" ]; then
  echo "PIN_ROOT is not set; see README.md" >&2
  exit 1
fi
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

echo "== 1. Build"
make -s
echo "== 2. Unit tests"
make -s unittest | tail -1
echo "== 3. Directed tests (logs in results/validation/)"
for t in t_cond t_calls t_indirect; do scripts/run_test.sh "$t" > /dev/null 2>&1; echo "  $t done"; done
echo "== 4. Replay check"
scripts/replay_check.sh t_calls | tail -1
echo "== 5. All workloads, all configurations"
scripts/run_all.sh > results/run_all_output.txt
echo "  done; tables in results/run_all_output.txt"
echo "== 6. Pattern test (bonus)"
scripts/run_patterns.sh > results/patterns_output.txt
echo "  done; table in results/patterns_output.txt"
echo "== 7. Offline indirect-branch model"
python3 scripts/model_interp_itp.py workloads/interp > results/model_interp_itp.txt
echo "  done; results/model_interp_itp.txt"
echo
echo "All results regenerated under results/."
