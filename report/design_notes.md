# Baseline design decisions

The spec fixes the baseline's structures and sizes (Section 4) but leaves
several behaviors open. These are the choices the simulator implements, with
the reason for each. They belong in the report's baseline section.

## 1. Simulation model

Each control-flow event, in program order, is predicted from its address
alone, scored against the actual outcome, and then used to update every
structure before the next event. There are no delayed updates and no
wrong-path execution, matching the spec's correct-path, trace-driven model.
(`pintool/bpred.cpp`, `ProcessEvent`)

## 2. Next-PC prediction

The BTB stores the branch type, so the front end learns from the BTB what
kind of instruction it is fetching, before decode.

| Case | Predicted next PC |
|---|---|
| BTB miss, any type | `pc + size` (looks like an ordinary instruction) |
| BTB hit, conditional | BTB target if the direction predictor says taken, else `pc + size` |
| BTB hit, direct or indirect jump or call | BTB target (for indirect: the most recent target) |
| BTB hit, return | top of the RAS; if the RAS is empty, the BTB target |

Simplification: the BTB is looked up only for control-flow instructions, so a
non-branch instruction falsely matching a partial tag is not modeled. The
course clarification allows the trace to omit non-branch instructions that
the predictor does not rely on. (`pintool/frontend.h`)

## 3. Misprediction accounting

- **Direction mispredictions:** conditional branches whose predicted
  direction was wrong. This measures the direction predictor alone and gives
  the conditional direction MPKI.
- **Wrong next PC:** control-flow instructions whose predicted next address
  was wrong. This is the total control-transfer MPKI used for the CPI
  estimate. Each one is charged to exactly one cause, so the causes sum to
  the total:
  - *return:* any mispredicted return;
  - *direction:* a conditional branch with the wrong direction;
  - *indirect target:* an indirect jump or call that hit in the BTB with the
    wrong target;
  - *BTB:* everything else, i.e. a taken branch that missed in the BTB, or a
    direct branch whose stored target was wrong.

A direction misprediction does not always cause a wrong next PC: if a branch
is predicted taken but misses in the BTB, the front end falls through anyway,
which is right when the branch is actually not taken. Such cases count toward
direction MPKI only. They are common while the BTB is cold: in the directed
tests, which are mostly start-up code, 276 of 2,239 direction mispredictions
were of this kind.

Every wrong next PC costs the same 15 cycles in the spec's CPI estimate,
although a real processor would correct a missed direct branch earlier, at
decode. (`pintool/stats.h`)

## 4. 64-bit addresses in 32-bit fields

BTB targets and RAS entries keep the low 32 bits of the address. On
prediction, the upper 32 bits are taken from the address of the branch being
predicted. This is exact whenever the branch and its target lie in the same
4 GiB region, which holds for every transfer within the program or within a
library. It fails for transfers between the program (near `0x400000`) and the
shared libraries (near `0x7fff...`): returns from library functions to the
program, calls through the PLT, and library callbacks into the program.

The simulator counts mispredictions where only the upper 32 bits were wrong
(`cross_region_only`), so results can be shown with and without this effect.
Widening the stored addresses is a candidate Section 5 improvement.
(`bp::Truncate`, `bp::Restore` in `pintool/predictor.h`)

## 5. BTB indexing, allocation and update

- Index: PC bits 7..0 (256 sets). x86 instructions can start at any byte, so
  the PC is not shifted.
- Tag: PC bits 23..8 (16 bits). Branches whose addresses agree in bits 23..0
  alias; the simulator counts these hits (`btb_alias_hits`) using a shadow
  copy of the full PC that is not part of the predictor's storage.
- Allocation only for taken branches: a never-taken branch needs no target.
- On every taken execution the entry's target and type are refreshed, so
  indirect branches predict their most recent target.
(`pintool/btb.h`)

## 6. Direction predictor

- Local history table indexed by PC bits 9..0. The 10-bit history indexes the
  local pattern table of 3-bit counters; taken if the counter is 4 or more.
- Global history holds the outcomes of the last 12 conditional branches only;
  unconditional transfers are always taken and would dilute it. It indexes
  the global pattern table and the choice table directly, as the spec says.
- The chooser is trained only when local and global disagree, toward the one
  that was right. Both pattern tables and both histories are updated on every
  conditional branch.
- Initial state: histories zero; counters at their weakest not-taken value
  (1 for 2-bit, 3 for 3-bit); the chooser weakly favoring local.
(`pintool/tournament.h`)

## 7. Return address stack

- The 5-bit field is the number of valid entries, 0..16.
- Every call (direct or indirect) pushes `pc + size`; every return pops. Both
  follow the actual program, whether or not the prediction was right.
- Overflow: a push onto a full stack discards the oldest entry (a
  shift-register stack in hardware; the code uses an equivalent circular
  buffer). Recursion deeper than 16 therefore predicts the innermost 16
  returns correctly.
- Underflow: an empty stack gives no prediction and the front end uses the
  BTB's stored target; the pop is ignored. For recursion this is usually
  right, since a recursive function's return goes to the same place every
  time but the last.
(`pintool/ras.h`)

## 8. Scope and storage

- All branches in the process are predicted, including the dynamic loader
  and shared libraries, and all executed instructions count toward MPKI
  (per the course clarification).
- Storage: 29,708 (direction) + 13,312 (BTB) + 517 (RAS) = 43,537 bits,
  matching the spec's table. A direct-mapped BTB needs no replacement state.
  The simulator's diagnostic counters and shadow PCs are not predictor state.
