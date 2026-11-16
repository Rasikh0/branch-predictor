# Section 5 improvements

Four independent changes, each motivated by a weakness the baseline showed on
the workloads, plus the combined final design. The second change was refined
after its first measurement, with an offline model to find the cause. Each change touches only its
own structure; everything else stays exactly the baseline, so each can be
measured on its own (`-predictor NAME`). The measurements come from
`scripts/run_all.sh`.

Baseline results that motivated them (wrong-next-PC MPKI by cause):

| Workload | Weakness | Baseline MPKI |
|---|---|---|
| gzip | direction mispredictions | 10.10 |
| interp | indirect target mispredictions | 52.65 |
| recurse | return mispredictions (192,081 RAS overflows) | 5.28 |
| gzip | BTB misses | 2.59 |

## 1. Direction: gshare-style global indexing (`gshare`, `gshare12`)

**Hypothesis.** The baseline indexes the global pattern table with the last
12 branch outcomes alone, so every branch that happens to see the same
recent history shares one counter. When those branches go different ways,
they keep overwriting each other's counter. XORing the branch address into
the index gives them separate counters. A longer history (14 bits) can also
capture longer correlations, and a 4x larger table keeps the extra index bits
from creating new collisions. This should help gzip and the start-up code,
whose many branches crowd the table. It cannot help much on `sort` and
`recurse`, whose hard branches compare against random data.

**Structures and logic.**
- Global pattern table: 16,384 x 2-bit counters, index = (PC XOR GHR) mod 2^14.
- GHR: 14 bits (conditional outcomes only, as before).
- Choice table: unchanged, 4,096 x 2-bit, indexed by the 12 most recent
  outcomes in the GHR.
- Local history and local pattern tables: unchanged.
- Lookup and update are otherwise the baseline's: the chooser is trained
  when local and global disagree, and both pattern tables and both histories
  are updated on every conditional branch.

**Storage.**

| Structure | Baseline | gshare |
|---|---|---|
| Local history table | 10,240 | 10,240 |
| Local pattern table | 3,072 | 3,072 |
| Global pattern table | 8,192 | 32,768 |
| Choice table | 8,192 | 8,192 |
| GHR | 12 | 14 |
| Direction total | 29,708 | 54,286 |
| Predictor total | 43,537 | 68,115 |

**Sub-experiment `gshare12`.** The same index change with the baseline's
sizes (12-bit history, 4,096-entry table, 0 extra bits). Comparing it with
`gshare` separates the effect of the hashing from the effect of the bigger
table and longer history.

(`pintool/tournament.h`)

## 2. Target: indirect target predictor (`itp`, refined as `itp4`, `itp4_gh`)

**Hypothesis.** The baseline BTB predicts an indirect jump or call's most
recent target, which is wrong whenever consecutive executions go to
different places. In `interp`, the dispatch goes to a different handler
almost every time, so nearly every one of its 3.5 million indirect branches
is mispredicted (52.6 MPKI). But the next handler is largely determined by
the handlers that just ran, because the bytecode program is fixed. A table
indexed by the branch address together with a history of recent indirect
targets can learn the sequence.

**First version (`itp`).**
- Path history register (PHR), 16 bits. On every indirect jump or call:
  PHR = ((PHR << 2) XOR mix(target)) mod 2^16, where
  mix(t) = t XOR (t >> 4) XOR (t >> 10) spreads aligned addresses' bits.
- Table: 512 entries, direct-mapped, each valid (1) + tag (8) + target (32).
  index = (PC XOR PHR) mod 512; tag = ((PC >> 9) XOR (PHR >> 9) XOR (PHR << 1)) mod 2^8.
  Targets are stored as low 32 bits and restored from the PC, as in the BTB.
- Lookup: if the BTB identifies the instruction as an indirect jump or call
  and this table hits, use this table's target; otherwise the BTB's.
- Update: on every indirect jump or call, write the actual target into the
  entry for (PC, PHR), then shift the actual target into the PHR.
- Storage: 512 x 41 + 16 = 21,008 bits. Predictor total 64,545.

Measured: `interp`'s indirect MPKI fell from 52.6 to 12.6, a 1.50 speedup,
but about a quarter of its indirect branches were still wrong.

**Diagnosis with an offline model.** `interp`'s indirect branches depend
only on the bytecode program and the compiled addresses, so
`scripts/model_interp_itp.py` replays its exact dispatch stream (3.5 million
branches) through Python copies of the predictor. The model reproduces the
Pin measurement for `itp` (841,375 wrong vs about 841,000 measured), and the
C++ predictor gives identical counts on the same stream. Results:

| Design (512 entries unless noted) | Wrong |
|---|---|
| `itp` as built: direct-mapped, index from the low 9 bits of PC XOR PHR | 841,375 |
| 1,024 entries, otherwise as built | 772,155 |
| direct-mapped, 16-bit context folded into the index | 331,237 |
| 4-way (128 sets), low index bits | 147,538 |
| 4-way, 10-bit context, every bit hashed into index and tag | 269,465 |
| 4-way, 12-bit context, every bit hashed | 266,478 |
| **`itp4`: 4-way, 14-bit context, every bit hashed** | **141,526** |
| 4-way, 16-bit context, every bit hashed | 201,786 |
| **`itp4_gh`: `itp4` plus 8 recent conditional outcomes** | **75,308** |
| ideal: unlimited table, exact last 4 targets | 263,490 |
| ideal: unlimited table, exact last 8 targets | 138,570 |
| ideal: unlimited table, exact last 12 targets | 132,610 |

Conclusions:
- `itp`'s remaining errors were not capacity (doubling the table barely
  helps) but contexts colliding: the index used only the low 9 bits of
  PC XOR PHR, and a direct-mapped table can't hold colliding contexts at once.
- History length must be chosen: 12 bits (about 6 targets) is too short to
  tell contexts apart, while 16 bits splits contexts that behave the same,
  leaving more entries to train and more collisions. This is the target
  predictor's version of "when can a longer history make prediction worse?"
  The 14-bit choice is tuned on `interp`, the only workload with many
  indirect branches.
- `itp4` comes within 2% of the ideal for 8 targets of history, so target
  history alone can't do much better. The remaining errors are data-dependent
  choices, mainly whether the bytecode takes the "x is even" path (`DIV`) or
  the odd one (`MUL`).
- In the function-pointer interpreter that choice is a real conditional
  branch (`op_jz`'s `jne`), so its outcome is in the direction predictor's
  global history. `itp4_gh` adds those outcomes to the context and halves
  the errors again. In the `switch` interpreter gcc compiled the choice to a
  branch-free `cmovne`, so no history can see it; those errors remain.

**Refined version (`itp4`).** Same 512 entries, organized as 128 sets x 4
ways with LRU (2-bit age per entry). Context H = PHR, 14 bits (about the
last 7 targets).
- index = (PC XOR H XOR (H >> 7)) mod 128
- tag = ((PC >> 7) XOR (H >> 7) XOR (H << 1)) mod 2^8

so every bit of H reaches both the index and the tag. Lookup and update as in
`itp`; replacement picks an invalid way, else the least recently used.
Storage: 512 x (1 + 8 + 32 + 2) + 14 = 22,030 bits. Predictor total 65,567.

**`itp4_gh`.** As `itp4`, with H = (PHR XOR (low 8 bits of GHR << 4)) mod 2^14,
where the GHR is the direction predictor's existing global history register,
so no storage is added. Predictor total 65,567.

(`pintool/itp.h`, used in `pintool/frontend.h`)

## 3. Return: deeper RAS (`ras64`)

**Hypothesis.** `recurse` builds a tree 37 levels deep, so the 16-entry RAS
overflows (192,081 times) and the outer returns fall back to the BTB, which
guesses the wrong call site about half the time because `insert` is called
from two places. A 64-entry RAS covers the whole depth.

**Structures and logic.** Unchanged except the size: 64 entries x 32 bits,
and the count grows to 7 bits (0..64). Same push, pop, overflow and
underflow behavior.

**Storage.** 64 x 32 + 7 = 2,055 bits (baseline 517). Predictor total 45,075.

(`pintool/ras.h`)

## 4. BTB: 4-way set-associative (`btb4way`)

**Hypothesis.** gzip has more branches than the 256-entry direct-mapped BTB
can hold, and branches whose addresses share the low 8 bits evict each other
even when other entries are unused. Four ways with LRU replacement remove
these conflicts while keeping the same number of entries.

**Structures and logic.**
- 64 sets x 4 ways = 256 entries, each valid (1) + tag (16) + target (32) +
  type (3) + LRU age (2).
- Index = PC bits 5..0; tag = PC bits 21..6. The tag stays 16 bits, so it now
  covers PC bits 21..0 instead of 23..0; any extra aliasing this causes shows
  up in `btb_alias_hits`.
- Replacement: an invalid way if there is one, else the least recently used.
  An entry becomes most recent when it is allocated or updated (taken
  branches).

**Storage.** 256 x 54 = 13,824 bits (baseline 13,312). Predictor total 44,049.

(`pintool/btb.h`)

## Final design (`final`)

`gshare` + `itp4_gh` + `ras64` + `btb4way`. (`final_v1` is the same with the
first-version `itp`, for comparison.)

| Structure | Bits |
|---|---|
| Direction predictor (gshare, 14-bit history) | 54,286 |
| BTB, 4-way | 13,824 |
| RAS, 64 entries | 2,055 |
| Indirect target predictor (4-way, uses the direction predictor's GHR) | 22,030 |
| **Total** | **92,195** |

That is within the 98,304-bit budget, with 6,109 bits to spare.
