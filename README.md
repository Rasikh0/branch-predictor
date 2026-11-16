# HW3: Branch Prediction with Pin

A Pin tool that observes every control-flow instruction of a running x86-64
program and simulates a front-end branch predictor (direction predictor, BTB
and return address stack) online, as the program runs.

## Layout

| Path | Contents |
|---|---|
| `pintool/` | The Pin tool (`bpred.cpp`), the predictor (`frontend.h`, `tournament.h`, `tage.h`, `btb.h`, `ras.h`, `itp.h`, `unified.h`), statistics (`stats.h`), shared definitions (`branch_event.h`, `predictor.h`), and the Pin build files reused from HW2 |
| `tests/` | Small directed test programs and their expected results (`EXPECTED.md`) |
| `tests/unit/` | Predictor unit tests and the log replay program; these build without Pin |
| `workloads/` | The workload programs for the experiments, and the generator for gzip's input |
| `scripts/` | Run and analysis scripts |
| `results/` | `validation/`: directed-test logs; `workloads/`: workload reports and summary tables |
| `report/` | The report; `design_notes.md`: baseline design decisions; `improvements.md`: Section 5 changes; `exploration.md`: Section 6; `bonus.md`: Section 9 |

## Setup (once per machine)

Requires an x86-64 Linux machine (we use an Ubuntu 24.04 CloudLab node) and
Pin 4.4 (kit 99977), downloaded from Intel's Pin page:

```bash
sudo apt-get update && sudo apt-get install -y build-essential
cd ~
wget https://software.intel.com/sites/landingpage/pintool/downloads/pin-external-4.4-99977-g54fbb8814-gcc-linux.tar.gz
tar xzf pin-external-4.4-99977-g54fbb8814-gcc-linux.tar.gz
echo 'export PIN_ROOT=$HOME/pin-external-4.4-99977-g54fbb8814-gcc-linux' >> ~/.bashrc
source ~/.bashrc
```

## Reproduce everything

```bash
scripts/reproduce_all.sh
```

Builds everything, runs the unit tests, the directed tests, the replay check,
every workload with every configuration, the pattern test and the offline
model, and writes all results under `results/` (about 5 to 10 minutes; the
first run downloads gzip 1.12). The individual steps are described below.

Rerunning in the same folder reproduces every number exactly. Running a copy
from a different folder can change gzip's results slightly (in a check, 41 more
instructions out of 152 million), which moves its MPKI in the third or fourth
decimal place; the other workloads stay identical.

## Build

From the top of this directory:

```bash
make            # builds pintool/obj-intel64/bpred.so and the directed tests
make unittest   # builds and runs the predictor unit tests (needs only g++)
```

## Run

General form:

```bash
setarch $(uname -m) -R $PIN_ROOT/pin -t pintool/obj-intel64/bpred.so [options] -- ./program
```

`setarch -R` turns off address randomization so addresses repeat between runs.

Tool options:

| Option | Default | Meaning |
|---|---|---|
| `-predictor NAME` | `baseline` | Predictor configuration (see "Predictor configurations" below) |
| `-o FILE` | `bpred.out` | End-of-run report (also printed to stderr) |
| `-max_insts N` | 0 (no limit) | Stop simulating after N instructions |
| `-max_events N` | 0 (no limit) | Stop simulating after N control-flow events |
| `-log FILE` | off | Write a CSV validation log of control-flow events |
| `-log_start N` | 0 | First event sequence number to log |
| `-log_count N` | 1000 | Maximum number of events to log |
| `-log_main 0/1` | 1 | Log only events in the main executable |
| `-top N` | 0 (off) | End the report with the N static branches with the most wrong next PCs |

## End-of-run report

The report has four parts: event counts by type (all code and the main
executable alone), the predictor's structures with their bit counts and the
budget check, the prediction results, and internal counts (RAS overflows and
underflows). The prediction results give, with counts and MPKI:

| Line | Meaning |
|---|---|
| `direction_mispredictions` | Conditional branches with the wrong predicted direction |
| `wrong_next_pc_total` | Control-flow instructions with the wrong predicted next address |
| `cause_direction`, `cause_btb_target`, `cause_indirect_target`, `cause_return` | The total split by cause; these sum to the total |
| `in_main_executable` | Wrong next PCs in the program itself, rather than the loader or libraries |
| `cross_region_only` | Wrong only in the upper 32 bits (see `report/design_notes.md`, item 4) |
| `btb_alias_hits` | BTB hits on an entry belonging to a different branch |
| `estimated_cpi` | 1 + 15 x (wrong-next-PC MPKI) / 1000 |

followed by the wrong-next-PC rate for each branch type.

## Validation log format

One row per control-flow event, in program order:

| Column | Meaning |
|---|---|
| `seq` | Event sequence number over the whole run, starting at 0 |
| `pc` | Address of the instruction |
| `size` | Instruction length in bytes |
| `type` | `cond`, `jmp_direct`, `jmp_indirect`, `call_direct`, `call_indirect` or `ret` |
| `taken` | 1 if taken (always 1 for jumps, calls and returns) |
| `next_pc` | Address actually executed next |
| `fallthrough_pc` | `pc + size` for conditional branches and calls (for a call, this is its return address); empty otherwise |
| `direct_target` | Target encoded in the instruction; empty for indirect jumps, indirect calls and returns |
| `inst_count` | Instructions executed so far, counting every instruction and including this one |
| `pred_next_pc` | The predictor's predicted next address |
| `pred_taken` | The direction predictor's guess (conditional branches only) |
| `btb_hit` | 1 if the BTB had an entry |
| `correct` | 1 if `pred_next_pc` equals `next_pc` |
| `image`, `routine` | File and function containing the instruction |
| `in_main` | 1 if the instruction is in the main executable (not the loader or a library) |
| `disasm` | The instruction, in the same syntax as `objdump -d` |

## Directed tests

```bash
scripts/run_test.sh t_cond
python3 scripts/summarize_log.py results/validation/t_cond.csv cond_test main
```

Compare the output with `tests/EXPECTED.md`. Repeat for `t_calls` and
`t_indirect`.

To check that a recorded log reproduces a run exactly:

```bash
scripts/replay_check.sh t_calls
```

This logs every event of the run (loader and libraries included), replays the
log through the predictor without Pin, and compares every prediction. It
should end with `Predictions that differ from the log: 0`. A grader can run
`tests/unit/replay results/validation/t_calls.full.csv` on the saved log with
no Pin installed.

## Workloads (spec Section 3.4)

| Workload | Spec category | What it does | Instructions |
|---|---|---|---|
| `loop` | (i) loop with predictable branches | 128 x 128 integer matrix multiply, twice | about 17 M |
| `sort` | (ii) sorting/searching, data-dependent branches | quicksort of 100,000 random keys, then 100,000 binary searches | about 41 M |
| `recurse` | (iii) recursion, nested calls/returns | recursive binary-search-tree insert (tree height 37, deeper than the RAS), recursive sum and height, recursive Fibonacci | about 24 M |
| `interp` | (iv) indirect dispatch | bytecode interpreter run twice: `switch` (jump table) and function-pointer table | about 67 M |
| `gzip` | nontrivial program | GNU gzip 1.12, built from source, `-6` on 500 KB of generated text | about 153 M |

Instruction counts are for the program alone, measured with valgrind; under
Pin each also includes about 110,000 start-up instructions. All data comes
from fixed seeds, so every run is identical. The programs are compiled with
`-O2` (real programs are optimized); `workloads/Makefile` also generates
`gzip_input.txt`, and downloads, checks (SHA-256) and builds gzip 1.12, so
the first `make` needs internet access and takes about 20 seconds more. gzip
is built from source rather than taken from the system so that every machine
runs the same code, compiled like the other workloads, with symbols for the
per-branch profile.

Run them all with one predictor and print the summary tables:

```bash
scripts/run_workloads.sh            # baseline; or give a predictor name
```

Reports go to `results/workloads/<workload>.<predictor>.txt`, and the tables
(control transfers by category, and prediction results) to
`results/workloads/summary.<predictor>.csv`. To print the tables again
without rerunning: `python3 scripts/collect_results.py baseline`.

Each report ends with the 20 static branches with the most wrong next PCs.
To print that part: `scripts/print_profile.sh <workload> <predictor> [rows]`.
To print the internal counts (RAS overflows, indirect predictor hits, unified
table contents): `scripts/print_diagnostics.sh <workload> <predictor>`.

So that instruction counts are identical on any machine and account, the
script runs the programs with a fixed, minimal environment (`env -i`), from
`workloads/` with relative paths, and always sends their output to a file
(`results/workloads/<workload>.out`): the loader and libc process every
environment variable, and libc runs different code for terminal and file
output.

## Predictor configurations (spec Section 5)

| Name | Change from the baseline | Bits |
|---|---|---|
| `baseline` | none (spec Section 4) | 43,537 |
| `gshare12` | global table indexed by PC XOR history, same sizes | 43,537 |
| `gshare` | global table indexed by PC XOR 14-bit history, 16K entries | 68,115 |
| `itp` | adds a 512-entry direct-mapped indirect target predictor using target history | 64,545 |
| `itp4` | refined: 4-way, 14-bit target history hashed into index and tag | 65,567 |
| `itp4_gh` | `itp4`, plus 8 recent conditional outcomes from the direction predictor's history | 65,567 |
| `ras64` | 64-entry RAS | 45,075 |
| `btb4way` | 4-way set-associative BTB with LRU | 44,049 |
| `final_v1` | `gshare` + `itp` + `ras64` + `btb4way` | 91,173 |
| `final` | `gshare` + `itp4_gh` + `ras64` + `btb4way` | 92,195 |
| `uni_hist` | Section 6: one 640-entry table for all targets, every lookup keyed by (PC, history) | 91,555 |
| `uni_phr` | Section 6: as `uni_hist`, history = target history only | 91,555 |
| `uni_2probe` | Section 6: one 640-entry table, PC probe then (PC, history) probe for indirect branches | 92,195 |
| `final_tour` | Section 9: `final` with the baseline tournament direction predictor | 67,617 |
| `tage_small` | Section 9: `final` with a TAGE-SC-L of about the tournament's size | 67,572 |
| `tage_base` | Section 9: `final` with TAGE only (no loop predictor, no SC) | 89,463 |
| `tage_l` | Section 9: `final` with TAGE + loop predictor | 90,078 |
| `tage` | Section 9: `final` with L-TAGE-SC in place of gshare | 96,252 |

The `uni_*` designs are the Section 6 exploration (option C, `report/exploration.md`); they use `final`'s direction predictor and RAS. The last five are the Section 9 bonus (`report/bonus.md`, `pintool/tage.h`); they keep `final`'s target side, so only the direction predictor differs. `scripts/run_patterns.sh` runs the bonus's branch-pattern test (`tests/t_patterns.c`) under each direction predictor and prints each pattern branch's error rate.

`report/improvements.md` gives each change's hypothesis, structures, lookup
and update logic, and bit accounting. The indirect predictor's refinement was
designed with `scripts/model_interp_itp.py`, an offline model that replays
`interp`'s exact indirect-branch stream (it reproduces the Pin measurement;
run it with `python3 scripts/model_interp_itp.py`, about 30 seconds).

Run every configuration on every workload and print the comparison
(wrong-next-PC MPKI, direction MPKI, estimated CPI, speedup over the baseline
with the geometric mean, and MPKI by cause):

```bash
scripts/run_all.sh
```

The comparison is also saved to `results/workloads/comparison.csv`. To print
it again without rerunning: `python3 scripts/compare_predictors.py`.

## Design notes

The baseline predictor's design decisions are in `report/design_notes.md`.

- Instructions are counted once per basic block, at the start of the block,
  so the count is exact at every branch event. A REP-prefixed string
  instruction counts as one instruction.
- Every instruction is counted, including the dynamic loader and shared
  libraries, as the assignment requires for MPKI.
- Control flow the predictor does not handle (far transfers, `xbegin`/`xend`,
  interrupts) is counted separately in the summary and not predicted.
