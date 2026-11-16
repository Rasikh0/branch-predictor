#!/usr/bin/env python3
"""
Compare predictor configurations across all workloads (spec Sections 5 and 7).

Usage:  python3 scripts/compare_predictors.py [predictor ...]
        (default: every configuration, baseline first)

Reads results/workloads/<workload>.<predictor>.txt and prints, for the
predictors named (default: all), per workload: storage, wrong-next-PC MPKI,
direction MPKI, estimated CPI, speedup over the baseline
(CPI_baseline / CPI_design) with the geometric mean across workloads, and
wrong-next-PC MPKI split by cause. Writes results/workloads/comparison.csv
with every configuration that has reports on disk, whichever ones are
printed, so a run of a few configurations doesn't drop the others.
"""
import csv
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from collect_results import WORKLOADS, parse_report  # noqa: E402

DEFAULT = ["baseline", "gshare12", "gshare", "itp", "itp4", "itp4_gh", "ras64", "btb4way",
           "final_v1", "final", "uni_hist", "uni_phr", "uni_2probe", "final_tour", "tage_small",
           "tage_base", "tage_l", "tage"]
CAUSES = [("cause_direction", "dir"), ("cause_btb_target", "btb"),
          ("cause_indirect_target", "indir"), ("cause_return", "ret")]


def main():
    preds = sys.argv[1:] or DEFAULT
    d = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "results", "workloads")
    res = {}
    for p in DEFAULT + [q for q in preds if q not in DEFAULT]:
        for w in WORKLOADS:
            path = os.path.join(d, f"{w}.{p}.txt")
            if os.path.exists(path):
                res[(p, w)] = parse_report(path)
    preds = [p for p in preds if any((p, w) in res for w in WORKLOADS)]
    if "baseline" not in preds:
        sys.exit("baseline results are needed for speedups; run scripts/run_workloads.sh baseline")

    def cell(p, w, key, fmt):
        r = res.get((p, w))
        return format(r[key], fmt) if r and key in r else "-"

    head = f"{'predictor':<10}" + "".join(f"{w:>10}" for w in WORKLOADS)

    def table(title, key, fmt, extra=None):
        print(title)
        print(head + (f"{extra[0]:>10}" if extra else ""))
        for p in preds:
            line = f"{p:<10}" + "".join(f"{cell(p, w, key, fmt):>10}" for w in WORKLOADS)
            print(line + (f"{extra[1](p):>10}" if extra else ""))
        print()

    def storage(p):
        for w in WORKLOADS:
            if (p, w) in res and "storage_bits" in res[(p, w)]:
                return f"{res[(p, w)]['storage_bits']:,}"
        return "-"

    def speedup(p, w):
        b, r = res.get(("baseline", w)), res.get((p, w))
        return b["estimated_cpi"] / r["estimated_cpi"] if b and r else None

    def geomean(p):
        s = [speedup(p, w) for w in WORKLOADS]
        s = [x for x in s if x]
        return format(math.exp(sum(math.log(x) for x in s) / len(s)), ".4f") if s else "-"

    table("Wrong-next-PC MPKI (storage in bits at right)", "wrong_next_pc_total_mpki", ".3f",
          ("bits", storage))
    table("Direction MPKI", "direction_mispredictions_mpki", ".3f")
    table("Estimated CPI = 1 + 15 x wrong-next-PC MPKI / 1000", "estimated_cpi", ".4f")

    print("Speedup over baseline = CPI_baseline / CPI_design")
    print(head + f"{'geomean':>10}")
    for p in preds:
        line = f"{p:<10}"
        for w in WORKLOADS:
            s = speedup(p, w)
            line += f"{format(s, '.4f') if s else '-':>10}"
        print(line + f"{geomean(p):>10}")
    print()

    print("Wrong-next-PC MPKI by cause")
    print(f"{'predictor':<10}{'cause':<7}" + "".join(f"{w:>10}" for w in WORKLOADS))
    for p in preds:
        for key, short in CAUSES:
            print(f"{p:<10}{short:<7}" + "".join(f"{cell(p, w, key + '_mpki', '.3f'):>10}"
                                                for w in WORKLOADS))
    print()

    rows = []
    for (p, w), r in sorted(res.items(), key=lambda kv: (DEFAULT.index(kv[0][0])
                                                          if kv[0][0] in DEFAULT else 99,
                                                          WORKLOADS.index(kv[0][1]))):
        row = dict(r, predictor=p, workload=w)
        s = speedup(p, w)
        if s:
            row["speedup"] = round(s, 6)
        rows.append(row)
    keys = ["predictor", "workload", "speedup"] + sorted(
        {k for r in rows for k in r} - {"predictor", "workload", "speedup"})
    out = os.path.join(d, "comparison.csv")
    with open(out, "w", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=keys)
        wr.writeheader()
        wr.writerows(rows)
    print(f"Wrote {os.path.normpath(out)}")


if __name__ == "__main__":
    main()
