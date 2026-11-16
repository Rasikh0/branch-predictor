/*
 * test_predictor.cpp -- unit tests for the baseline predictor and statistics.
 *
 * Runs without Pin. Each test feeds a hand-made sequence of control-flow
 * events through the predictor and checks a result worked out by hand.
 *
 * Build and run:  make -C tests/unit run     (or: make unittest from the top)
 */
#include <stdint.h>
#include <iostream>
#include <vector>

#include "btb.h"
#include "frontend.h"
#include "ras.h"
#include "stats.h"
#include "tage.h"
#include "tournament.h"

static int g_failures = 0;

#define CHECK_EQ(actual, expected, what)                                          \
  do {                                                                            \
    unsigned long long a_ = (unsigned long long)(actual);                         \
    unsigned long long e_ = (unsigned long long)(expected);                       \
    std::cout << (a_ == e_ ? "  PASS  " : "  FAIL  ") << what << ": got " << a_   \
              << ", expected " << e_ << "\n";                                     \
    if (a_ != e_) g_failures++;                                                   \
  } while (0)

static StaticBranchInfo Info(uint64_t pc, uint32_t size, BranchType type)
{
  StaticBranchInfo s;
  s.pc = pc;
  s.size = size;
  s.type = type;
  s.direct_target = 0;
  s.fallthrough = (type == BR_COND || type == BR_CALL_DIRECT || type == BR_CALL_INDIRECT)
                  ? pc + size : 0;
  s.in_main = true;
  return s;
}

/* One event through predict -> record -> update. Returns true if mispredicted. */
static bool Run(FrontEndPredictor &fe, Stats &st, const StaticBranchInfo &s, bool taken,
                uint64_t target)
{
  static uint64_t seq = 0;
  BranchEvent ev;
  ev.seq = seq++;
  ev.inst_count = seq * 5;
  ev.info = &s;
  ev.taken = taken;
  ev.next_pc = taken ? target : s.pc + s.size;
  Prediction p = fe.Predict(s.pc, s.pc + s.size);
  bool wrong = st.Record(ev, p);
  fe.Update(ev, p);
  return wrong;
}

static void TestStorage()
{
  std::cout << "Storage (spec Section 4 table)\n";
  CHECK_EQ(TournamentPredictor().StorageBits(), 29708, "direction predictor bits");
  CHECK_EQ(Btb().StorageBits(), 13312, "BTB bits");
  CHECK_EQ(Ras().StorageBits(), 517, "RAS bits");
  FrontEnd fe{FrontEndConfig()};
  CHECK_EQ(fe.StorageBits(), 43537, "baseline total bits");
}

/* Mispredictions of one conditional branch over the last `window` of `n`
   outcomes of a repeating pattern (1 = taken). */
static uint64_t DirectionMisses(const std::vector<int> &pattern, int n, int window)
{
  TournamentPredictor t;
  uint64_t pc = 0x401187, misses = 0;
  for (int i = 0; i < n; i++) {
    bool taken = pattern[i % pattern.size()] != 0;
    if (i >= n - window && t.Predict(pc) != taken) misses++;
    t.Update(pc, taken);
  }
  return misses;
}

static void TestDirection()
{
  std::cout << "Direction predictor, one branch, after warm-up\n";
  CHECK_EQ(DirectionMisses({1}, 1000, 900), 0, "always taken");
  CHECK_EQ(DirectionMisses({1, 0}, 1000, 900), 0, "alternating T,N (a 2-bit counter alone would miss these)");
  std::vector<int> loop8(8, 1);
  loop8[7] = 0;
  CHECK_EQ(DirectionMisses(loop8, 4000, 800), 0, "loop of 8 (history covers the whole period)");
  /* 15 taken then 1 not taken: the last 10 (local) or 12 (global) outcomes
     look the same before the exit as before an ordinary iteration, so the
     exit is mispredicted once per period. 800 outcomes = 50 periods. */
  std::vector<int> loop16(16, 1);
  loop16[15] = 0;
  CHECK_EQ(DirectionMisses(loop16, 4000, 800), 50, "loop of 16 (longer than the histories): 1 miss per period");
}

static void TestBtb()
{
  std::cout << "BTB\n";
  {
    FrontEnd fe{FrontEndConfig()};
    Stats st;
    StaticBranchInfo nt = Info(0x401000, 2, BR_COND);
    for (int i = 0; i < 20; i++) Run(fe, st, nt, false, 0x401100);
    CHECK_EQ(fe.Predict(0x401000, 0x401002).btb_hit, 0, "never-taken branch gets no BTB entry");
    CHECK_EQ(st.WrongNextPc(), 0, "never-taken branch: no wrong next PC");
  }
  {
    FrontEnd fe{FrontEndConfig()};
    Stats st;
    StaticBranchInfo j = Info(0x401145, 2, BR_JMP_DIRECT);
    for (int i = 0; i < 10; i++) Run(fe, st, j, true, 0x401183);
    CHECK_EQ(st.WrongNextPc(), 1, "direct jump: only the first (cold) execution is wrong");
    CHECK_EQ(st.Cause(Stats::CAUSE_BTB), 1, "  ...charged to the BTB");
  }
  {
    /* A jump from the program (0x4...) into a library (0x7fff...): only the low
       32 bits of the target are stored, so it is restored with the jump's own
       upper bits, 0x0, and is always wrong. */
    FrontEnd fe{FrontEndConfig()};
    Stats st;
    StaticBranchInfo plt = Info(0x401044, 6, BR_JMP_INDIRECT);
    for (int i = 0; i < 10; i++) Run(fe, st, plt, true, 0x7fffb4250e00ULL);
    CHECK_EQ(st.WrongNextPc(), 10, "jump into a library: wrong every time with 32-bit targets");
    CHECK_EQ(st.CrossRegion(), 9, "  ...9 of them only because the upper 32 bits were lost");
  }
  {
    Btb b;
    b.Update(0x401010, 0x401100, BR_JMP_DIRECT);
    BtbResult r = b.Lookup(0x401010 + (1ULL << 24));
    CHECK_EQ(r.hit, 1, "a PC 16 MiB away matches the 16-bit partial tag");
    CHECK_EQ(r.alias, 1, "  ...and is flagged as an alias");
  }
  {
    Btb b(256, 4);  /* 64 sets x 4 ways */
    CHECK_EQ(b.StorageBits(), 256 * (52 + 2), "4-way BTB adds 2 LRU bits per entry");
    for (int i = 0; i < 4; i++) b.Update(0x400000 + i * 64, 0x500000 + i, BR_JMP_DIRECT);
    b.Update(0x400000, 0x500000, BR_JMP_DIRECT);           /* touch the first again */
    b.Update(0x400000 + 4 * 64, 0x500004, BR_JMP_DIRECT);  /* evicts the LRU one: i = 1 */
    CHECK_EQ(b.Lookup(0x400000).hit, 1, "4-way LRU keeps the recently used entry");
    CHECK_EQ(b.Lookup(0x400000 + 64).hit, 0, "4-way LRU evicts the least recently used entry");
  }
}

static void TestRas()
{
  std::cout << "RAS\n";
  Ras r;
  for (int i = 0; i < 20; i++) r.Push(0x401000 + i * 5);
  int correct = 0, empty = 0;
  for (int i = 19; i >= 0; i--) {
    uint64_t a;
    if (!r.Top(0x401200, &a)) empty++;
    else if (a == 0x401000ULL + i * 5) correct++;
    r.Pop();
  }
  CHECK_EQ(correct, 16, "20 nested calls: the innermost 16 returns come back right");
  CHECK_EQ(empty, 4, "  ...the outer 4 find the stack empty (oldest entries were dropped)");

  Ras r2;
  uint64_t a = 0;
  r2.Push(0x401005);
  r2.Top(0x7fff00001000ULL, &a);
  CHECK_EQ(a == 0x401005, 0, "return from a library into the program: upper bits come out wrong");
}

static void TestIndirectDispatch()
{
  /* t_indirect's dispatch(6): one indirect call rotating among 3 targets,
     each callee returning to the instruction after the call. */
  std::cout << "End to end: t_indirect dispatch loop\n";
  FrontEnd fe{FrontEndConfig()};
  Stats st;
  StaticBranchInfo call = Info(0x4011cc, 2, BR_CALL_INDIRECT);
  StaticBranchInfo rets[3] = {Info(0x401148, 1, BR_RET), Info(0x40115a, 1, BR_RET),
                              Info(0x40116c, 1, BR_RET)};
  uint64_t targets[3] = {0x401136, 0x401149, 0x40115b};
  for (int i = 0; i < 6; i++) {
    Run(fe, st, call, true, targets[i % 3]);
    Run(fe, st, rets[i % 3], true, 0x4011ce);
  }
  CHECK_EQ(st.WrongByType(BR_CALL_INDIRECT), 6,
           "indirect call: wrong all 6 times (last target is never the next one)");
  CHECK_EQ(st.Cause(Stats::CAUSE_BTB), 1, "  ...first one is a BTB miss");
  CHECK_EQ(st.Cause(Stats::CAUSE_INDIRECT), 5, "  ...other 5 hit with the wrong target");
  CHECK_EQ(st.WrongByType(BR_RET), 3, "returns: only each callee's first (cold) return is wrong");
}

static void TestDeepRecursion()
{
  /* fact(20)-style recursion, 20 calls deep: deeper than the 16-entry RAS. */
  std::cout << "End to end: recursion 20 deep\n";
  FrontEnd fe{FrontEndConfig()};
  Stats st;
  StaticBranchInfo from_main = Info(0x4011e4, 5, BR_CALL_DIRECT);
  StaticBranchInfo recurse   = Info(0x4011bb, 5, BR_CALL_DIRECT);
  StaticBranchInfo ret       = Info(0x4011c5, 1, BR_RET);
  Run(fe, st, from_main, true, 0x401197);
  for (int i = 0; i < 19; i++) Run(fe, st, recurse, true, 0x401197);
  for (int i = 0; i < 19; i++) Run(fe, st, ret, true, 0x4011c0);
  Run(fe, st, ret, true, 0x4011e9);
  /* Calls: each call site's first execution misses the BTB (2).
     Returns: the first is a BTB miss; 2-16 come from the RAS; 17-19 find it
     empty and use the BTB's last target (0x4011c0, right); the 20th also
     uses 0x4011c0 but should go back to main (wrong). */
  CHECK_EQ(st.WrongByType(BR_CALL_DIRECT), 2, "calls: one cold miss per call site");
  CHECK_EQ(st.WrongByType(BR_RET), 2, "returns: first (cold) and last (RAS empty, BTB fallback wrong)");
  CHECK_EQ(st.Cause(Stats::CAUSE_DIRECTION) + st.Cause(Stats::CAUSE_BTB) +
           st.Cause(Stats::CAUSE_INDIRECT) + st.Cause(Stats::CAUSE_RETURN),
           st.WrongNextPc(), "causes add up to the total");
}

static void TestImprovementStorage()
{
  std::cout << "Storage of the Section 5 configurations\n";
  struct { const char *name; uint64_t bits; } cases[] = {
    {"baseline", 43537},
    {"gshare12", 43537},   /* same tables, only the index changes */
    {"gshare",   68115},   /* GPT 16384 x 2 = 32768 instead of 8192; GHR 14 bits */
    {"itp",      64545},   /* + 512 x 41 + 16 = 21008 */
    {"itp4",     65567},   /* + 512 x (41 + 2 LRU) + 14 = 22030 */
    {"itp4_gh",  65567},   /* same table; reuses the direction predictor's GHR */
    {"ras64",    45075},   /* RAS 64 x 32 + 7 = 2055 instead of 517 */
    {"btb4way",  44049},   /* + 256 x 2 LRU bits = 512 */
    {"final_v1", 91173},   /* 54286 + 13824 + 2055 + 21008 */
    {"final",    92195},   /* 54286 + 13824 + 2055 + 22030 */
    {"uni_hist", 91555},   /* 54286 + 2055 + 640 x 55 + 14 */
    {"uni_phr",  91555},
    {"uni_2probe", 92195}, /* 54286 + 2055 + 640 x 56 + 14: same bits as final */
  };
  for (auto &c : cases) {
    FrontEndPredictor *p = CreatePredictor(c.name);
    CHECK_EQ(p ? p->StorageBits() : 0, c.bits, std::string(c.name) + " bits");
    delete p;
  }
  FrontEndPredictor *f = CreatePredictor("final");
  CHECK_EQ(f->StorageBits() <= kStorageBudgetBits, 1, "final design is within the 98,304-bit budget");
  delete f;
}

/* Two branches see the same global history (12 always-taken branches before
   each) but go opposite ways. Indexed by history alone, they share one
   global counter, which the always-taken branches also train, so it stays at
   "taken" and the not-taken branch is wrong every time. Indexed by PC XOR
   history, each branch gets its own counter. */
static uint64_t GlobalComponentMisses(bool pc_hash)
{
  TournamentPredictor t(1024, 10, 3, 12, 2, 2, 12, 12, pc_hash);
  uint64_t misses = 0;
  for (int i = 0; i < 400; i++) {
    for (int f = 0; f < 12; f++) t.Update(0x402000 + f * 8, true);
    uint64_t pc = (i % 2) ? 0x401100 : 0x401234;
    bool taken = (i % 2) != 0;
    if (i >= 200 && t.PredictGlobal(pc) != taken) misses++;
    t.Update(pc, taken);
  }
  return misses;
}

static void TestImprovements()
{
  std::cout << "Section 5 improvements, behavior\n";
  CHECK_EQ(GlobalComponentMisses(false), 100,
           "1 gshare: baseline global table, the not-taken branch loses every time (100 of 200 wrong)");
  CHECK_EQ(GlobalComponentMisses(true), 0,
           "1 gshare: PC XOR history separates them (0 wrong)");

  {
    /* The t_indirect dispatch pattern, run long enough to warm up. */
    uint64_t wrong[2];
    const char *names[2] = {"baseline", "itp"};
    for (int k = 0; k < 2; k++) {
      FrontEndPredictor *fe = CreatePredictor(names[k]);
      Stats st;
      StaticBranchInfo call = Info(0x4011cc, 2, BR_CALL_INDIRECT);
      StaticBranchInfo rets[3] = {Info(0x401148, 1, BR_RET), Info(0x40115a, 1, BR_RET),
                                  Info(0x40116c, 1, BR_RET)};
      uint64_t targets[3] = {0x401136, 0x401149, 0x40115b};
      for (int i = 0; i < 300; i++) {
        if (i == 150) st = Stats();
        Run(*fe, st, call, true, targets[i % 3]);
        Run(*fe, st, rets[i % 3], true, 0x4011ce);
      }
      wrong[k] = st.WrongByType(BR_CALL_INDIRECT);
      delete fe;
    }
    CHECK_EQ(wrong[0], 150, "2 itp: baseline, rotating indirect call wrong every time (150 of 150)");
    CHECK_EQ(wrong[1], 0, "2 itp: with target history, learned after warm-up (0 of 150)");
  }

  {
    /* Interpreter-like: a data-dependent conditional branch C (like the
       bytecode's "is x even?") is followed by 5 always-taken branches and
       two dispatches from the same indirect jump. The first dispatch always
       goes to T1; the second goes to A if C was taken, else B. Before the
       second dispatch the target history is the same either way, so itp4 can
       only guess; the GHR holds C's outcome, so itp4_gh can learn it. */
    uint64_t wrong[2];
    const char *names[2] = {"itp4", "itp4_gh"};
    for (int k = 0; k < 2; k++) {
      FrontEndPredictor *fe = CreatePredictor(names[k]);
      Stats st;
      StaticBranchInfo c = Info(0x401500, 2, BR_COND);
      StaticBranchInfo fill[5];
      for (int f = 0; f < 5; f++) fill[f] = Info(0x401600 + f * 4, 2, BR_COND);
      StaticBranchInfo disp = Info(0x401722, 3, BR_JMP_INDIRECT);
      uint64_t lcg = 12345;
      for (int i = 0; i < 2000; i++) {
        if (i == 1000) st = Stats();
        lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
        bool taken = (lcg >> 33) & 1;
        Run(*fe, st, c, taken, 0x401540);
        for (int f = 0; f < 5; f++) Run(*fe, st, fill[f], true, 0x401600 + f * 4 + 4);
        Run(*fe, st, disp, true, 0x401800);                        /* T1 */
        Run(*fe, st, disp, true, taken ? 0x401900 : 0x401a00);     /* A or B */
      }
      wrong[k] = st.WrongByType(BR_JMP_INDIRECT);
      delete fe;
    }
    CHECK_EQ(wrong[0] > 300, 1,
             "2 itp4_gh: target history only (itp4), the outcome-dependent dispatch is a guess (>300 of 2000 wrong)");
    CHECK_EQ(wrong[1], 0, "2 itp4_gh: with the GHR in the context, learned (0 of 2000 wrong)");
  }

  {
    /* 40-deep recursion, deeper than 16 but within 64. Each level's return
       goes to one of two call sites, like recurse.c's insert(). */
    uint64_t wrong[2];
    const char *names[2] = {"baseline", "ras64"};
    for (int k = 0; k < 2; k++) {
      FrontEndPredictor *fe = CreatePredictor(names[k]);
      Stats st;
      StaticBranchInfo left  = Info(0x401290, 5, BR_CALL_DIRECT);
      StaticBranchInfo right = Info(0x4012b0, 5, BR_CALL_DIRECT);
      StaticBranchInfo ret   = Info(0x4012d0, 1, BR_RET);
      for (int rep = 0; rep < 20; rep++) {
        if (rep == 10) st = Stats();
        bool side[40];
        for (int d = 0; d < 40; d++) {
          side[d] = ((d * 7 + rep) % 3) == 0;
          Run(*fe, st, side[d] ? left : right, true, 0x401270);
        }
        for (int d = 39; d >= 0; d--) Run(*fe, st, ret, true, side[d] ? 0x401295 : 0x4012b5);
      }
      wrong[k] = st.WrongByType(BR_RET);
      delete fe;
    }
    CHECK_EQ(wrong[0] > 0, 1, "3 ras64: baseline 16-entry RAS mispredicts some of the outer 24 returns");
    CHECK_EQ(wrong[1], 0, "3 ras64: 64 entries cover all 40 levels (0 wrong)");
  }

  {
    /* Four jumps whose addresses share the low 8 bits: in the direct-mapped
       BTB they compete for one entry; in the 4-way BTB (64 sets) they share
       one set and all fit. */
    uint64_t wrong[2];
    const char *names[2] = {"baseline", "btb4way"};
    for (int k = 0; k < 2; k++) {
      FrontEndPredictor *fe = CreatePredictor(names[k]);
      Stats st;
      StaticBranchInfo j[4];
      for (int b = 0; b < 4; b++) j[b] = Info(0x401040 + b * 0x1000, 2, BR_JMP_DIRECT);
      for (int i = 0; i < 400; i++) {
        if (i == 40) st = Stats();
        Run(*fe, st, j[i % 4], true, 0x402000 + (i % 4) * 16);
      }
      wrong[k] = st.WrongNextPc();
      delete fe;
    }
    CHECK_EQ(wrong[0], 360, "4 btb4way: direct-mapped, 4 jumps fighting over one entry (all 360 wrong)");
    CHECK_EQ(wrong[1], 0, "4 btb4way: 4-way set holds all four (0 wrong)");
  }
}

static void TestUnified()
{
  std::cout << "Section 6 option C: unified target table\n";
  /* A direct jump J runs after 8 random conditional branches, so the GHR
     holds a different random pattern almost every time (256 possible). */
  const char *names[3] = {"uni_hist", "uni_phr", "uni_2probe"};
  uint64_t wrong[3];
  for (int k = 0; k < 3; k++) {
    FrontEndPredictor *fe = CreatePredictor(names[k]);
    Stats st;
    StaticBranchInfo c[8];
    for (int b = 0; b < 8; b++) c[b] = Info(0x401500 + b * 16, 2, BR_COND);
    StaticBranchInfo j = Info(0x401700, 2, BR_JMP_DIRECT);
    uint64_t lcg = 99;
    for (int i = 0; i < 1000; i++) {
      for (int b = 0; b < 8; b++) {
        lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
        bool t = (lcg >> 33) & 1;
        Run(*fe, st, c[b], t, 0x401500 + b * 16 + 8);
      }
      Run(*fe, st, j, true, 0x401800);
    }
    wrong[k] = st.WrongByType(BR_JMP_DIRECT);
    delete fe;
  }
  CHECK_EQ(wrong[0] > 200, 1,
           "uni_hist: one direct jump needs an entry per history pattern (>200 cold misses of 1000)");
  CHECK_EQ(wrong[1], 1, "uni_phr: target history doesn't change here, so one entry (1 miss)");
  CHECK_EQ(wrong[2], 1, "uni_2probe: direct branches use the PC entry only (1 miss)");

  /* Indirect behavior should match itp4_gh: the rotating call is learned. */
  FrontEndPredictor *fe = CreatePredictor("uni_2probe");
  Stats st;
  StaticBranchInfo call = Info(0x4011cc, 2, BR_CALL_INDIRECT);
  StaticBranchInfo rets[3] = {Info(0x401148, 1, BR_RET), Info(0x40115a, 1, BR_RET),
                              Info(0x40116c, 1, BR_RET)};
  uint64_t targets[3] = {0x401136, 0x401149, 0x40115b};
  for (int i = 0; i < 300; i++) {
    if (i == 150) st = Stats();
    Run(*fe, st, call, true, targets[i % 3]);
    Run(*fe, st, rets[i % 3], true, 0x4011ce);
  }
  CHECK_EQ(st.WrongByType(BR_CALL_INDIRECT), 0, "uni_2probe: rotating indirect call learned (0 of 150)");
  delete fe;
}

/* ---- Section 9 bonus: TAGE-SC-L ---------------------------------------- */

static DirectionPredictor *MakeDir(const std::string &k)
{
  if (k == "tournament") return new TournamentPredictor();
  TageConfig c;
  c.use_loop = (k != "tage_base");
  c.use_sc = (k == "tage");
  return new TagePredictor(c);
}

/* Loop branch with n executions per instance (n-1 taken, then the exit),
   one always-taken branch between instances. Misses in instances 200-299. */
static uint64_t LoopMisses(const std::string &k, int n)
{
  DirectionPredictor *d = MakeDir(k);
  uint64_t m = 0;
  for (int inst = 0; inst < 300; inst++) {
    for (int i = 0; i < n; i++) {
      bool t = i < n - 1;
      if (inst >= 200 && d->Predict(0x401100) != t) m++;
      d->Update(0x401100, t);
    }
    d->Update(0x401200, true);
  }
  delete d;
  return m;
}

/* Branch Y repeats random branch X's outcome, `gap` always-taken branches
   later. Misses of Y in the last 2000 of 4000. */
static uint64_t CorrMisses(const std::string &k, int gap)
{
  DirectionPredictor *d = MakeDir(k);
  uint64_t m = 0, s = 7;
  for (int i = 0; i < 4000; i++) {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    bool x = (s >> 33) & 1;
    d->Update(0x401300, x);
    for (int f = 0; f < gap; f++) d->Update(0x401400 + 8 * f, true);
    if (i >= 2000 && d->Predict(0x401380) != x) m++;
    d->Update(0x401380, x);
  }
  delete d;
  return m;
}

/* A branch taken 85% of the time at random, after 6 random branches.
   Misses in the last 20000 of 40000; the best possible is about 3000. */
static uint64_t BiasedMisses(const std::string &k)
{
  DirectionPredictor *d = MakeDir(k);
  uint64_t m = 0, s = 11;
  for (int i = 0; i < 40000; i++) {
    for (int f = 0; f < 6; f++) {
      s = s * 6364136223846793005ULL + 1442695040888963407ULL;
      d->Update(0x401500 + 8 * f, (s >> 33) & 1);
    }
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    bool t = ((s >> 33) % 100) < 85;
    if (i >= 20000 && d->Predict(0x401600) != t) m++;
    d->Update(0x401600, t);
  }
  delete d;
  return m;
}

static void TestTage()
{
  std::cout << "Section 9 bonus: TAGE-SC-L\n";
  struct { const char *name; uint64_t bits; } cases[] = {
    {"final_tour", 67617},  /* 29708 tournament + 37909 final target side */
    {"tage_small", 67572},  /* 29663 TAGE-SC-L, about the tournament's size */
    {"tage_base",  89463},  /* 51554: bimodal 8192 + tagged 43008 + histories 176 + folded 156 + 22 */
    {"tage_l",     90078},  /* + loop predictor 615 */
    {"tage",       96252},  /* + statistical corrector 6158 + its folded histories 16 */
  };
  for (auto &c : cases) {
    FrontEndPredictor *p = CreatePredictor(c.name);
    CHECK_EQ(p ? p->StorageBits() : 0, c.bits, std::string(c.name) + " bits");
    delete p;
  }
  FrontEndPredictor *t = CreatePredictor("tage");
  CHECK_EQ(t->StorageBits() <= kStorageBudgetBits, 1, "tage (final design with TAGE-SC-L) is within budget");
  delete t;

  CHECK_EQ(LoopMisses("tournament", 50), 100, "loop of 50: tournament misses every exit (100 of 100)");
  CHECK_EQ(LoopMisses("tage_base", 50), 0, "loop of 50: TAGE's 77/160-outcome tables see the exit (0)");
  CHECK_EQ(LoopMisses("tage_base", 300), 100, "loop of 300: longer than TAGE's 160-outcome history (100 of 100)");
  CHECK_EQ(LoopMisses("tage_l", 300), 0, "loop of 300: the loop predictor counts it (0)");
  CHECK_EQ(CorrMisses("tournament", 30) > 500, 1, "outcome 30 branches back: tournament can't see it (>500 of 2000)");
  CHECK_EQ(CorrMisses("tage_base", 30), 0, "outcome 30 branches back: TAGE's 37-outcome table sees it (0)");
  uint64_t tour = BiasedMisses("tournament"), tl = BiasedMisses("tage_l"), full = BiasedMisses("tage");
  CHECK_EQ(tour > 5000, 1, "random 85%-taken branch: tournament >25% wrong (random history scatters it)");
  CHECK_EQ(tl <= 3100, 1, "random 85%-taken branch: TAGE near the 15% floor");
  CHECK_EQ(full <= tl, 1, "random 85%-taken branch: the statistical corrector doesn't make it worse");
}

int main()
{
  TestStorage();
  TestImprovementStorage();
  TestImprovements();
  TestUnified();
  TestTage();
  TestDirection();
  TestBtb();
  TestRas();
  TestIndirectDispatch();
  TestDeepRecursion();
  std::cout << (g_failures ? "\nFAILED: " : "\nAll tests passed") ;
  if (g_failures) std::cout << g_failures << " check(s)";
  std::cout << "\n";
  return g_failures ? 1 : 0;
}
