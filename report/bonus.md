# Section 9 (bonus): L-TAGE-SC

Implementation: `pintool/tage.h`. Configurations (all with the final
design's target side, i.e. 4-way BTB, `itp4_gh` and a 64-entry RAS, so only
the direction predictor differs):

| Configuration | Direction predictor | Direction bits | Total bits |
|---|---|---|---|
| `final_tour` | baseline tournament | 29,708 | 67,617 |
| `final` | gshare tournament (best direction predictor so far) | 54,286 | 92,195 |
| `tage_small` | L-TAGE-SC, about the tournament's size | 29,663 | 67,572 |
| `tage_base` | TAGE only (no loop predictor, no SC) | 51,554 | 89,463 |
| `tage_l` | TAGE + loop predictor | 52,169 | 90,078 |
| `tage` | TAGE + loop predictor + statistical corrector | 58,343 | 96,252 |

`tage` fits the 98,304-bit budget. `tage_small` against `final_tour`, and
`tage` against `final`, compare at nearly equal direction-predictor storage.
`tage_base` and `tage_l` are ablations that measure each component's
contribution.

## 1. Role of each component

- **Tagged geometric-history tables (TAGE).** Several tables, each indexed
  by a hash of the PC and a different length of global history, from very
  short (4 outcomes) to very long (160). Every entry carries a partial tag,
  so a table only answers if the entry really belongs to this branch in this
  history context. The prediction comes from the matching table with the
  longest history, which captures the most specific correlation available.
  Short tables warm up quickly and cover branches that need little history;
  long tables are only used for branches that need them, because entries are
  allocated in longer tables only after a misprediction. This avoids
  gshare's dilemma of one history length for every branch.
- **Loop predictor.** Learns a branch's trip count (up to 1,023) and
  predicts the exit. A loop whose trip count exceeds the longest history
  (160) looks identical to TAGE before every iteration, so TAGE always
  misses the exit; the loop predictor counts iterations instead of
  remembering them.
- **Statistical corrector (SC).** Small tables of signed counters, indexed by
  the PC with the prediction so far and by short global histories, that
  learn when the prediction so far tends to be wrong for a branch in a
  context. It targets branches that are statistically biased but not
  correlated with history, where TAGE's tagged entries keep being allocated
  and replaced and can do worse than simply following the bias.

**How they combine.** TAGE predicts first (provider, or alternate for a
newly allocated provider). The loop predictor overrides when it is confident
(three identical trip counts in a row) and has proven itself (USE_LOOP >= 0).
The SC then overrides the result if its counters disagree with enough
margin for that prediction's confidence.

## 2. Specification and storage

| Structure | Parameters | Bits |
|---|---|---|
| Bimodal base | 4,096 x 2-bit counters, index PC mod 4096 | 8,192 |
| Tagged tables | 6 tables x 512 entries; entry = 3-bit counter + 2-bit u + tag; history lengths 4, 8, 17, 37, 77, 160; tags 8, 8, 9, 9, 10, 10 bits | 43,008 |
| Global history | 160 conditional outcomes | 160 |
| Path history | 16 bits, 1 address bit per conditional branch | 16 |
| Folded-history registers | per table: index (9) + two tag folds (t, t-1) | 156 |
| USE_ALT | 4-bit counter | 4 |
| Aging tick | 18-bit counter: all u halved every 2^18 conditional branches | 18 |
| Loop predictor | 16 entries (4 sets x 4 ways) x (valid 1, tag 10, trip count 10, iteration 10, confidence 2, age 4, direction 1 = 38) + 7-bit USE_LOOP | 615 |
| SC bias table | 512 x 6-bit signed, index (PC, prediction so far) | 3,072 |
| SC history tables | 2 x 256 x 6-bit signed, histories 10 and 20 | 3,072 |
| SC folded histories | 2 x 8 | 16 |
| SC threshold | theta (8) + TC (6) | 14 |
| **Total** | | **58,343** |

**Index and tag functions** (table i, L_i = history length, F = folded
history of the newest L_i outcomes):
- index = (PC XOR (PC >> (9 - i)) XOR F_index(L_i, 9 bits) XOR P) mod 512,
  where P folds min(L_i, 16) path-history bits to 9 bits;
- tag = (PC XOR F_tag(L_i, t_i bits) XOR (F_tag(L_i, t_i - 1 bits) << 1)) mod 2^t_i.

**Counter widths and rules.** Tagged entries: 3-bit prediction counter
(taken if >= 4), 2-bit usefulness u. Provider = longest matching table;
alternate = next match or bimodal. A newly allocated (weak: 3 or 4) provider
defers to the alternate while USE_ALT >= 8. Update: the provider's counter
moves toward the outcome (the alternate's too if the provider's u is 0); u
moves toward the provider when provider and alternate disagreed; on a TAGE
misprediction, allocate one entry in the shortest longer table with u = 0
(weak counter toward the outcome), or else decrement those entries' u.
Loop predictor and SC rules are in `pintool/tage.h`'s header comment.

**Simplifications versus Seznec's TAGE-SC-L.** One deterministic allocation
per misprediction (not randomized among candidates); no local-history or
IMLI components in the SC; no speculative history repair (the simulator is
correct-path, so histories always hold actual outcomes).

## 3. Implementation and testing

TAGE-SC-L replaces the tournament predictor behind the same direction-
predictor interface, so it sees the same correct-path Pin event stream and
is scored by the same statistics as every other configuration. Its history
also supplies the 8 recent outcomes the indirect predictor (`itp4_gh`) uses.

Unit tests (`tests/unit/test_predictor.cpp`) check every configuration's
storage and each component's purpose:

| Pattern | Tournament | TAGE only | TAGE + loop | TAGE + loop + SC |
|---|---|---|---|---|
| loop of 50: exit misses (of 100 instances) | 100 | 0 | 0 | 0 |
| loop of 300, longer than the 160-outcome history (of 100) | 100 | 100 | 0 | 0 |
| outcome repeats a random branch 30 branches earlier (of 2,000) | 715 | 0 | 0 | 0 |
| random branch, 85% taken (of 20,000; best possible about 3,000) | 5,688 | 3,026 | 3,026 | 2,985 |

On real code, `tests/t_patterns.c` puts the same patterns in an actual
program, run under Pin by `scripts/run_patterns.sh`.

## 4. Results and explanation

From `scripts/run_all.sh baseline final_tour final tage_small tage_base tage_l tage`,
`scripts/run_patterns.sh`, the per-branch profiles and TAGE's component counts.

**Direction MPKI**

| Configuration | Direction bits | loop | sort | recurse | interp | gzip |
|---|---|---|---|---|---|---|
| `final_tour` (tournament) | 29,708 | 0.635 | 43.143 | 21.558 | 0.503 | 10.588 |
| `final` (gshare) | 54,286 | 0.635 | 42.819 | 21.467 | 0.526 | 10.455 |
| `tage_small` | 29,663 | 0.116 | 41.121 | 21.794 | 0.448 | 10.104 |
| `tage_base` | 51,554 | 0.600 | 41.519 | 20.971 | 0.446 | 10.244 |
| `tage_l` | 52,169 | 0.105 | 41.519 | 20.971 | 0.446 | 10.085 |
| `tage` | 58,343 | 0.112 | 41.033 | 20.894 | 0.448 | 9.925 |

**Total wrong-next-PC MPKI and geometric-mean speedup over the baseline**

| Configuration | loop | sort | recurse | interp | gzip | Geomean speedup |
|---|---|---|---|---|---|---|
| `final_tour` | 0.682 | 43.162 | 21.590 | 1.647 | 10.604 | 1.1346 |
| `final` | 0.685 | 42.839 | 21.500 | 1.671 | 10.482 | 1.1357 |
| `tage_small` | 0.166 | 41.142 | 21.829 | 1.593 | 10.132 | 1.1415 |
| `tage_base` | 0.652 | 41.540 | 21.009 | 1.591 | 10.271 | 1.1407 |
| `tage_l` | 0.158 | 41.540 | 21.010 | 1.591 | 10.113 | 1.1429 |
| `tage` | 0.162 | 41.053 | 20.930 | 1.592 | 9.953 | 1.1446 |

`tage` has the best result overall (1.1446). `tage_small`, at the
tournament's size, beats gshare at nearly twice the storage (1.1415 vs
1.1357).

**Pattern test (`tests/t_patterns.c`, wrong %)**

| Pattern branch | Bias % | Tournament | gshare | TAGE only | + loop | + SC |
|---|---|---|---|---|---|---|
| `loop_50` back edge | 2.0 | 2.00 | 2.00 | 0.00 | 0.00 | 0.00 |
| `loop_300` back edge | 0.3 | 0.37 | 0.33 | 0.34 | 0.00 | 0.00 |
| `corr_30` X (random) | 49.9 | 50.40 | 50.50 | 49.77 | 49.76 | 49.14 |
| `corr_30` 30-iteration loop | 3.3 | 3.33 | 3.33 | 0.00 | 0.00 | 0.00 |
| `corr_30` Y (repeats X) | 49.9 | 33.73 | 37.37 | 0.00 | 0.04 | 0.07 |
| `biased_85` | 15.0 | 21.79 | 17.34 | 15.40 | 15.40 | 15.04 |
| `random_50` | 49.8 | 49.92 | 50.21 | 50.31 | 50.31 | 50.52 |

### Why TAGE-SC-L wins or loses, by pattern

- **Loops longer than the tournament's history.** The tournament misses
  every exit of a 50-iteration loop (2.00% = 1 in 50); TAGE's 77- and
  160-outcome tables see the previous exit and predict it perfectly. A
  300-iteration loop exceeds even TAGE's longest history (0.34%, every exit
  missed) and needs the loop predictor (0.00%). On the `loop` workload,
  TAGE alone barely helps (0.600 vs 0.635 MPKI), but the loop predictor
  overrode TAGE 8,436 times and was right every time, which almost exactly
  matches the 8,437 mispredictions of TAGE's 160-outcome table: direction
  MPKI falls by 83%, to 0.105.
- **Correlation beyond 12 outcomes.** In `corr_30`, branch Y repeats a
  random branch 31 branches earlier. The tournament's 12-outcome history
  can't reach it (34-37% wrong); TAGE's 37-outcome table gets it right
  (0.00%).
- **Biased, history-independent branches.** For `biased_85`, random history
  scatters the tournament's training across many counters (21.8% wrong, far
  above the 15% floor). TAGE falls back to its base table (15.4%), and the SC
  closes the rest of the gap (15.04%). On gzip, SC overrides were right
  99,703 times and wrong 75,286 times, a net 24,417 fewer mispredictions
  (exactly the 0.16 MPKI difference between `tage_l` and `tage`); on sort,
  a net 20,016 fewer (0.49 MPKI).
- **Data-dependent branches: no predictor helps.** The pattern test's random
  branches stay at about 50% for everything. In the workloads, sort's binary
  search branch is 50.0% wrong under TAGE (bias 48.9%), the same as before.
  TAGE's own component counts show it can't find structure there: in sort,
  tables 1-3 provide most predictions and are 21-29% wrong. Its sort gain
  (43.1 to 41.0 MPKI) comes from quicksort's partition loops, each a few
  points better than under the tournament (for example 0x40136c: 35.1% vs
  37.2%), where runs of taken iterations have some regularity.
- **gzip: modest gain, mostly from long histories and the add-ons.** Direction
  MPKI falls 5% versus gshare (10.455 to 9.925): about 2% from TAGE itself
  (10.244), 1.5% from the loop predictor (28,316 overrides, 26,204 right), 1.5%
  from the SC. Its tagged tables with 17-77 outcomes provide most predictions,
  so long histories are used. But the dominant `longest_match` branch only
  improves from 7.1% to 6.9% wrong (bias 7.6%): it compares input bytes, so
  it is data-dependent.
- **Where TAGE-SC-L loses: local-history correlation.** gzip's branch
  0x4040ff in `deflate` is 20.7% wrong under the baseline tournament but
  43.1% under TAGE-SC-L, essentially its bias (43.3%). Its outcome follows a
  pattern in its *own* history, which the tournament's local-history
  component captures; TAGE uses only global history, where that pattern is
  buried among other branches' data-dependent outcomes. Seznec's full
  TAGE-SC-L includes local-history components in the SC for exactly this
  case; our simplified SC omits them, and this branch shows the cost.
- **Size matters for recursion.** `tage_small` is worse than the tournament
  on `recurse` (21.79 vs 21.56 MPKI): with 256-entry tables, the many
  history contexts of the random tree descent crowd each other out, while
  the full-size `tage` is better (20.89).
- **The loop predictor occasionally misfires.** In `interp`, the loop
  predictor overrode TAGE 1,769 times and was right only 869 times
  (Collatz loops have varying trip counts). USE_LOOP keeps such overrides
  rare, so the net effect is negligible (0.446 MPKI with or without it).
