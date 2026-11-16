#!/usr/bin/env python3
"""
Offline model of the interp workload's indirect branches, used to diagnose
and refine the indirect target predictor (report/improvements.md, item 2).

Usage:  python3 scripts/model_interp_itp.py [path/to/interp]   (takes a few minutes)

interp's indirect branches depend only on the bytecode program and the
compiled addresses, so they can be replayed exactly without Pin:
  - run_switch: one jump-table dispatch (`notrack jmp *%rax`) per bytecode op
  - run_fnptr:  one indirect call through the handler table per bytecode op
The script reads those addresses from the binary (objdump, nm, the jump table
in .rodata), runs the same bytecode program as interp.c, and replays the
dispatch stream through Python copies of the indirect target predictor
designs (same hashing as pintool/itp.h; the BTB is modeled as "most recent
target per PC"). The direction history seen by each dispatch is modeled from
the interpreters' conditional branches: in run_switch only a bounds check
that is never taken (gcc compiles the bytecode branch to cmov); in run_fnptr
op_jz's branch and the loop branch.

The as-built design reproduces the Pin measurement (about 841,000 wrong),
which validates the model. The C++ predictor gives identical counts on the
same stream.
"""
import re
import struct
import subprocess
import sys

BIN = sys.argv[1] if len(sys.argv) > 1 else "workloads/interp"
PUSH, LOAD, STORE, ADD, MUL, DIV, MOD, LT, NE, JZ, JMP, HALT = range(12)
HANDLERS = ["op_push", "op_load", "op_store", "op_add", "op_mul", "op_div",
            "op_mod", "op_lt", "op_ne", "op_jz", "op_jmp", "op_halt"]


def disasm(func):
    out = subprocess.run(["objdump", "-d", "--no-show-raw-insn", BIN],
                         capture_output=True, text=True, check=True).stdout
    m = re.search(rf"<{func}>:\n(.*?)\n\n", out, re.S)
    return m.group(1)


def addresses():
    syms = {}
    for line in subprocess.run(["nm", BIN], capture_output=True, text=True).stdout.split("\n"):
        p = line.split()
        if len(p) == 3:
            syms[p[2]] = int(p[0], 16)
    handler = [syms[h] for h in HANDLERS]
    sw = disasm("run_switch")
    switch_pc = int(re.search(r"^\s*([0-9a-f]+):\s+notrack jmp \*%rax", sw, re.M).group(1), 16)
    table = int(re.search(r"lea\s+0x[0-9a-f]+\(%rip\),%rsi\s+# ([0-9a-f]+)", sw).group(1), 16)
    fp = disasm("run_fnptr")
    fnptr_pc = int(re.search(r"^\s*([0-9a-f]+):\s+call\s+\*", fp, re.M).group(1), 16)
    # Read the jump table (12 signed 32-bit offsets from the table base).
    secs = subprocess.run(["readelf", "-SW", BIN], capture_output=True, text=True).stdout
    m = re.search(r"\.rodata\s+PROGBITS\s+([0-9a-f]+)\s+([0-9a-f]+)", secs)
    vaddr, off = int(m.group(1), 16), int(m.group(2), 16)
    data = open(BIN, "rb").read()
    case = [table + struct.unpack_from("<i", data, off + table - vaddr + 4 * k)[0]
            for k in range(12)]
    return switch_pc, case, fnptr_pc, handler


def build(limit):
    prog = []

    def emit(op, arg=0):
        prog.append([op, arg])
        return len(prog) - 1
    emit(PUSH, 1); emit(STORE, 0)
    outer = len(prog)
    emit(LOAD, 0); emit(PUSH, limit + 1); emit(LT); to_end = emit(JZ)
    emit(LOAD, 0); emit(STORE, 1); emit(PUSH, 0); emit(STORE, 2)
    inner = len(prog)
    emit(LOAD, 1); emit(PUSH, 1); emit(NE); to_done = emit(JZ)
    emit(LOAD, 1); emit(PUSH, 2); emit(MOD); to_even = emit(JZ)
    emit(LOAD, 1); emit(PUSH, 3); emit(MUL); emit(PUSH, 1); emit(ADD); emit(STORE, 1)
    to_step = emit(JMP)
    prog[to_even][1] = len(prog)
    emit(LOAD, 1); emit(PUSH, 2); emit(DIV); emit(STORE, 1)
    prog[to_step][1] = len(prog)
    emit(LOAD, 2); emit(PUSH, 1); emit(ADD); emit(STORE, 2); emit(JMP, inner)
    prog[to_done][1] = len(prog)
    emit(LOAD, 3); emit(LOAD, 2); emit(ADD); emit(STORE, 3)
    emit(LOAD, 0); emit(PUSH, 1); emit(ADD); emit(STORE, 0); emit(JMP, outer)
    prog[to_end][1] = len(prog)
    emit(HALT)
    return prog


def run(prog):
    """Execute the bytecode; return [(op, jz_value_nonzero or None)] and the total."""
    st, v, pc, ops = [], [0] * 4, 0, []
    while True:
        op, a = prog[pc]
        pc += 1
        ops.append((op, st[-1] != 0 if op == JZ else None))
        if op == PUSH: st.append(a)
        elif op == LOAD: st.append(v[a])
        elif op == STORE: v[a] = st.pop()
        elif op in (ADD, MUL, DIV, MOD, LT, NE):
            b = st.pop(); x = st.pop()
            st.append({ADD: lambda: x + b, MUL: lambda: x * b, DIV: lambda: x // b,
                       MOD: lambda: x % b, LT: lambda: int(x < b),
                       NE: lambda: int(x != b)}[op]())
        elif op == JZ:
            if st.pop() == 0: pc = a
        elif op == JMP: pc = a
        elif op == HALT: return ops, v[3]


def make_events():
    switch_pc, case, fnptr_pc, handler = addresses()
    ops, total = run(build(1500))
    assert total == 95708, "bytecode result differs from interp.c"
    events, ghr = [], 0
    for o, _ in ops:                       # run_switch: bounds check (not taken), dispatch
        ghr = (ghr << 1) & 0xfff
        events.append((switch_pc, case[o], ghr))
    for o, nz in ops:                      # run_fnptr: dispatch, op_jz's jne, loop je
        events.append((fnptr_pc, handler[o], ghr))
        if o == JZ:
            ghr = ((ghr << 1) | int(nz)) & 0xfff
        ghr = ((ghr << 1) | int(o != HALT)) & 0xfff
    return events


def mix(t):
    return t ^ (t >> 4) ^ (t >> 10)


def simulate(events, entries=512, ways=1, tag_bits=8, hist_bits=16, fold=False, ghr_bits=0):
    """Same design as pintool/itp.h; returns the number of wrong predictions."""
    sets = entries // ways
    ib = sets.bit_length() - 1
    table = [[None] * ways for _ in range(sets)]      # entry: [tag, target, lru age]
    phr, last, wrong = 0, {}, 0
    hmask = (1 << hist_bits) - 1
    for pc, t, g in events:
        h = (phr ^ ((g & ((1 << ghr_bits) - 1)) << 4)) & hmask
        x = pc ^ h
        if fold:
            sh = ib
            while sh < hist_bits:
                x ^= h >> sh
                sh += ib
        row = table[x & (sets - 1)]
        tg = (pc >> ib) ^ (h >> ib) ^ (h << 1)
        if fold:
            tg ^= h >> (ib + tag_bits)
        tag = tg & ((1 << tag_bits) - 1)
        hit = next((e for e in row if e and e[0] == tag), None)
        if (hit[1] if hit else last.get(pc)) != t:
            wrong += 1
        e = hit
        if e is None:
            free = [i for i, y in enumerate(row) if y is None]
            i = free[0] if free else max(range(ways), key=lambda k: row[k][2])
            e = row[i] = [tag, t, ways]
        old = e[2]
        for y in row:
            if y is not None and y is not e and y[2] < old:
                y[2] += 1
        e[0], e[1], e[2] = tag, t, 0
        last[pc] = t
        phr = ((phr << 2) ^ mix(t)) & hmask
    return wrong


def ideal(events, k):
    """Unlimited table keyed by the exact last k targets: the best target history can do."""
    hist, table, last, wrong = (), {}, {}, 0
    for pc, t, _ in events:
        if table.get((pc, hist), last.get(pc)) != t:
            wrong += 1
        table[(pc, hist)] = t
        last[pc] = t
        hist = (hist + (t,))[-k:]
    return wrong


def main():
    events = make_events()
    print(f"Modeled {len(events):,} indirect branches "
          f"(Pin measured 3,507,088 indirect jumps and calls in interp)\n")
    rows = [
        ("itp as built: 512 x 1-way, 16-bit context, low index bits", dict()),
        ("1024 x 1-way, otherwise as built", dict(entries=1024)),
        ("512 x 1-way, 16-bit context folded into index", dict(fold=True, hist_bits=16)),
        ("128 sets x 4-way, 16-bit context, low index bits", dict(ways=4)),
        ("128 sets x 4-way, 10-bit context, all bits hashed", dict(ways=4, fold=True, hist_bits=10)),
        ("128 sets x 4-way, 12-bit context, all bits hashed", dict(ways=4, fold=True, hist_bits=12)),
        ("itp4: 128 sets x 4-way, 14-bit context", dict(ways=4, fold=True, hist_bits=14)),
        ("128 sets x 4-way, 16-bit context, all bits hashed", dict(ways=4, fold=True, hist_bits=16)),
        ("itp4_gh: itp4 + 8 conditional outcomes (GHR)",
         dict(ways=4, fold=True, hist_bits=14, ghr_bits=8)),
    ]
    print(f"{'design':<60}{'wrong':>10}")
    for desc, kw in rows:
        print(f"{desc:<60}{simulate(events, **kw):>10,}", flush=True)
    for k in (4, 8, 12):
        print(f"{'ideal: unlimited table, exact last %d targets' % k:<60}{ideal(events, k):>10,}",
              flush=True)


if __name__ == "__main__":
    main()
