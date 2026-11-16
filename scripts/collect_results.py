#!/usr/bin/env python3
"""
Collect the Pin tool's end-of-run reports into tables and a CSV file.

Usage:  python3 scripts/collect_results.py [predictor] [results-dir]
        (defaults: baseline, results/workloads)

Reads <results-dir>/<workload>.<predictor>.txt for every workload, prints
  Table 1  control-transfer counts by category (spec Section 3.4)
  Table 2  prediction results (spec Section 4.2)
and writes all the numbers to <results-dir>/summary.<predictor>.csv.
"""
import csv
import os
import re
import sys

WORKLOADS = ["loop", "sort", "recurse", "interp", "gzip"]
TYPES = ["cond", "jmp_direct", "jmp_indirect", "call_direct", "call_indirect", "ret"]
METRICS = ["direction_mispredictions", "wrong_next_pc_total", "cause_direction",
           "cause_btb_target", "cause_indirect_target", "cause_return",
           "in_main_executable", "cross_region_only", "btb_alias_hits"]


def parse_report(path):
    r = {}
    section = None
    for line in open(path):
        t = line.split()
        if not t:
            continue
        if line.startswith("  Instructions executed:"):
            r["instructions"] = int(t[2])
        elif line.startswith("  Control-flow events:"):
            r["events"] = int(t[2])
        elif t[0] == "type" and "all" in t:
            section = "counts"
        elif line.startswith("  Wrong next PC by type"):
            section = "by_type"
        elif section == "counts" and t[0] in TYPES + ["total"] and len(t) == 3:
            r[f"{t[0]}_events"] = int(t[1])
            r[f"{t[0]}_events_main"] = int(t[2])
        elif section == "counts" and t[:2] == ["cond", "taken"]:
            r["cond_taken"] = int(t[2])
        elif section == "by_type" and t[0] in TYPES and len(t) == 4:
            r[f"{t[0]}_wrong"] = int(t[2])
        elif t[0] in METRICS and len(t) == 3:
            r[t[0]] = int(t[1])
            r[t[0] + "_mpki"] = float(t[2])
        elif t[0] in ("conditional_branches", "direction_accuracy_percent", "estimated_cpi"):
            r[t[0]] = float(t[1])
        elif t[0] == "Total" and "budget" in line:
            r["storage_bits"] = int(t[1])
        elif line.startswith("  RAS pushes"):
            nums = [int(x) for x in re.findall(r"\d+", line)]
            r["ras_overflows"], r["ras_underflows"] = nums[2], nums[3]
    return r


def main():
    pred = sys.argv[1] if len(sys.argv) > 1 else "baseline"
    d = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "results", "workloads")
    rows = []
    for w in WORKLOADS:
        path = os.path.join(d, f"{w}.{pred}.txt")
        if os.path.exists(path):
            r = parse_report(path)
            r["workload"], r["predictor"] = w, pred
            rows.append(r)
        else:
            print(f"(no report for {w}: {path})")
    if not rows:
        sys.exit("no reports found")

    print(f"Table 1: control transfers by category, all code (predictor run: {pred})")
    print(f"{'workload':<9}{'instructions':>14}{'cond':>13}{'taken%':>8}{'jmp_dir':>12}"
          f"{'jmp_ind':>12}{'call_dir':>12}{'call_ind':>12}{'ret':>12}{'total':>13}")
    for r in rows:
        cond = r.get("cond_events", 0)
        taken = 100.0 * r.get("cond_taken", 0) / cond if cond else 0.0
        print(f"{r['workload']:<9}{r['instructions']:>14,}{cond:>13,}{taken:>8.1f}"
              f"{r['jmp_direct_events']:>12,}{r['jmp_indirect_events']:>12,}"
              f"{r['call_direct_events']:>12,}{r['call_indirect_events']:>12,}"
              f"{r['ret_events']:>12,}{r['total_events']:>13,}")

    print(f"\nTable 2: prediction results, MPKI (predictor: {pred}, "
          f"{rows[0].get('storage_bits', '?')} bits)")
    print(f"{'workload':<9}{'direction':>10}{'wrongPC':>9}{'=dir':>7}{'+btb':>7}{'+indir':>8}"
          f"{'+ret':>7}{'CPI':>8}{'xregion':>9}{'RASovfl':>9}")
    for r in rows:
        print(f"{r['workload']:<9}{r['direction_mispredictions_mpki']:>10.3f}"
              f"{r['wrong_next_pc_total_mpki']:>9.3f}{r['cause_direction_mpki']:>7.3f}"
              f"{r['cause_btb_target_mpki']:>7.3f}{r['cause_indirect_target_mpki']:>8.3f}"
              f"{r['cause_return_mpki']:>7.3f}{r['estimated_cpi']:>8.4f}"
              f"{r['cross_region_only']:>9,}{r.get('ras_overflows', 0):>9,}")
    print("  wrongPC = dir + btb + indir + ret.  xregion = wrong only in the upper 32 bits"
          " (count).  RASovfl = RAS overflows (count).")

    keys = ["workload", "predictor"] + sorted({k for r in rows for k in r} - {"workload", "predictor"})
    out = os.path.join(d, f"summary.{pred}.csv")
    with open(out, "w", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=keys)
        wr.writeheader()
        wr.writerows(rows)
    print(f"\nWrote {os.path.normpath(out)}")


if __name__ == "__main__":
    main()
