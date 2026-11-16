#!/usr/bin/env python3
"""
Summarize a bpred CSV event log per static instruction, for checking the
directed tests against tests/EXPECTED.md.

Usage:  python3 scripts/summarize_log.py results/validation/t_cond.csv [routine ...]

For each control-flow instruction in the log, prints how many times it ran,
how many times it was taken, how many times the predicted next PC was wrong
(if the log has predictions), and the next-PC values it produced (in order of
first appearance). Optional routine names restrict the output.
"""
import csv
import sys
from collections import OrderedDict


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    wanted = set(sys.argv[2:])
    rows = OrderedDict()
    with open(sys.argv[1], newline="") as f:
        for r in csv.DictReader(f):
            if wanted and r["routine"] not in wanted:
                continue
            key = int(r["pc"], 16)
            s = rows.setdefault(key, {"routine": r["routine"], "type": r["type"],
                                      "size": r["size"], "disasm": r["disasm"],
                                      "execs": 0, "taken": 0, "wrong": 0, "next": []})
            s["execs"] += 1
            s["taken"] += int(r["taken"])
            if r.get("correct") == "0":
                s["wrong"] += 1
            if r["next_pc"] not in s["next"]:
                s["next"].append(r["next_pc"])

    print(f"{'routine':<14} {'pc':<10} {'size':>4} {'type':<14} {'execs':>5} {'taken':>5} "
          f"{'wrong':>5}  {'instruction':<30} next_pc values")
    for pc in sorted(rows):
        s = rows[pc]
        nxt = ", ".join(s["next"][:6]) + (" ..." if len(s["next"]) > 6 else "")
        print(f"{s['routine'][:14]:<14} {hex(pc):<10} {s['size']:>4} {s['type']:<14} "
              f"{s['execs']:>5} {s['taken']:>5} {s['wrong']:>5}  {s['disasm'][:30]:<30} {nxt}")


if __name__ == "__main__":
    main()
