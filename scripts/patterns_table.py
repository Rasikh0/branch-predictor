#!/usr/bin/env python3
"""
Table for the Section 9 pattern test: for each pattern branch in
tests/t_patterns.c, its wrong% under each predictor.

Usage: python3 scripts/patterns_table.py [predictor ...]
Reads results/patterns/t_patterns.<predictor>.txt (written by run_patterns.sh).
A branch missing from a report's top-40 profile had no mispredictions.
"""
import os
import re
import sys

FUNCS = ["loop_50", "loop_300", "corr_30", "biased_85", "random_50"]
ROW = re.compile(r"^\s+\d+\s+(0x[0-9a-f]+)\s+(\S+)\s+(\d+)\s+([\d.]+)\s+(\d+)\s+(\d+)\s+"
                 r"([\d.]+)\s+(\S+)\s+([\d.]+)\s+([\d.]+)\s+([^:\s]+):(\S+)\s*$")


def profile(path):
    rows = {}
    for line in open(path):
        m = ROW.match(line)
        if m and m.group(11) == "t_patterns" and m.group(12) in FUNCS:
            rows[m.group(1)] = dict(func=m.group(12), execs=int(m.group(3)),
                                    taken=float(m.group(4)), wrong=int(m.group(6)),
                                    bias=m.group(8))
    return rows


def direction_mpki(path):
    for line in open(path):
        t = line.split()
        if t and t[0] == "direction_mispredictions":
            return float(t[2])
    return float("nan")


def main():
    preds = sys.argv[1:] or ["final_tour", "final", "tage_base", "tage_l", "tage"]
    d = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "results", "patterns")
    prof = {p: profile(os.path.join(d, f"t_patterns.{p}.txt")) for p in preds}
    branches = {}
    for p in preds:
        for pc, r in prof[p].items():
            branches.setdefault(pc, r)
    order = sorted(branches, key=lambda pc: (FUNCS.index(branches[pc]["func"]), int(pc, 16)))
    print("Wrong% per pattern branch (bias% = error of always guessing the common direction)")
    print(f"{'function':<11}{'pc':<11}{'executed':>10}{'taken%':>8}{'bias%':>7}"
          + "".join(f"{p:>12}" for p in preds))
    for pc in order:
        b = branches[pc]
        cells = []
        for p in preds:
            r = prof[p].get(pc)
            cells.append(f"{100.0 * r['wrong'] / b['execs'] if r else 0.0:>12.2f}")
        print(f"{b['func']:<11}{pc:<11}{b['execs']:>10,}{b['taken']:>8.1f}{b['bias']:>7}"
              + "".join(cells))
    print(f"{'whole program: direction MPKI':<47}"
          + "".join(f"{direction_mpki(os.path.join(d, f't_patterns.{p}.txt')):>12.3f}"
                    for p in preds))


if __name__ == "__main__":
    main()
