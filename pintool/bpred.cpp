/*
 * bpred.cpp -- Pin tool for HW3 (branch prediction).
 *
 * Watches every dynamically executed control-flow instruction (conditional
 * branches, jumps, calls and returns), counts every executed instruction, and
 * hands each control-flow event to the predictor simulator as it happens
 * ("online" simulation, in the style of the HW2 cache simulator). Nothing is
 * written to disk except an optional small CSV validation log and the
 * end-of-run report.
 *
 * Build:  make obj-intel64/bpred.so        (PIN_ROOT must be set)
 * Run:    $PIN_ROOT/pin -t obj-intel64/bpred.so [options] -- ./program
 */
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

#include "pin.H"
#include "branch_event.h"
#include "frontend.h"
#include "stats.h"

using std::string;

/* ========================================================================== */
/* Command-line options                                                       */
/* ========================================================================== */

KNOB<string> KnobPredictor(KNOB_MODE_WRITEONCE, "pintool",
    "predictor", "baseline", "Predictor configuration to simulate");
KNOB<string> KnobOutput(KNOB_MODE_WRITEONCE, "pintool",
    "o", "bpred.out", "File for the end-of-run summary (it is also printed to stderr)");
KNOB<UINT64> KnobMaxInsts(KNOB_MODE_WRITEONCE, "pintool",
    "max_insts", "0", "Stop simulating after this many instructions (0 = no limit)");
KNOB<UINT64> KnobMaxEvents(KNOB_MODE_WRITEONCE, "pintool",
    "max_events", "0", "Stop simulating after this many control-flow events (0 = no limit)");
KNOB<string> KnobLog(KNOB_MODE_WRITEONCE, "pintool",
    "log", "", "Write a CSV validation log of control-flow events to this file");
KNOB<UINT64> KnobLogStart(KNOB_MODE_WRITEONCE, "pintool",
    "log_start", "0", "Sequence number of the first event to log");
KNOB<UINT64> KnobLogCount(KNOB_MODE_WRITEONCE, "pintool",
    "log_count", "1000", "Maximum number of events to log");
KNOB<bool>   KnobLogMainOnly(KNOB_MODE_WRITEONCE, "pintool",
    "log_main", "true", "Log only events in the main executable (skip the loader and libraries)");
KNOB<UINT32> KnobTop(KNOB_MODE_WRITEONCE, "pintool",
    "top", "0", "Add the N branches with the most wrong next PCs to the report (0 = off)");

/* ========================================================================== */
/* State                                                                      */
/* ========================================================================== */

static UINT64 g_insts   = 0;      /* instructions executed (all of them, branches or not) */
static UINT64 g_events  = 0;      /* control-flow events seen = next sequence number */
static bool   g_stopped = false;  /* set once an instruction or event limit is reached */
static UINT64 g_max_insts = 0, g_max_events = 0;

/* Event counts by type, for all code and for the main executable only. */
static UINT64 g_count[BR_NUM_TYPES];
static UINT64 g_count_main[BR_NUM_TYPES];
static UINT64 g_cond_taken = 0, g_cond_taken_main = 0;
static UINT64 g_other = 0;        /* control flow we don't predict: far, xbegin/xend, etc. */
static bool   g_multithreaded = false;

/* The simulated predictor and its statistics. */
static FrontEndPredictor *g_pred = 0;
static Stats              g_stats;

/* Validation log. */
static std::ofstream g_log;
static bool   g_log_on = false, g_log_main_only = true;
static UINT64 g_log_start = 0, g_log_count = 0, g_logged = 0;

/* One StaticBranchInfo per static instruction address. Pin can instrument the
   same instruction more than once (e.g. when it appears in several traces), so
   reuse the existing record instead of creating a duplicate. */
static std::map<ADDRINT, StaticBranchInfo *> g_static;
static UINT32 g_next_id = 0;

/* ========================================================================== */
/* Helpers                                                                    */
/* ========================================================================== */

static string Hex(UINT64 v)
{
  std::ostringstream o;
  o << "0x" << std::hex << v;
  return o.str();
}

static string BaseName(const string &path)
{
  string::size_type slash = path.rfind('/');
  return (slash == string::npos) ? path : path.substr(slash + 1);
}

/* ========================================================================== */
/* Per-event processing                                                       */
/* ========================================================================== */

/* CSV columns: seq,pc,size,type,taken,next_pc,fallthrough_pc,direct_target,
                inst_count,pred_next_pc,pred_taken,btb_hit,correct,
                image,in_main,routine,disasm */
static void LogEvent(const BranchEvent &ev, const Prediction &p, bool wrong)
{
  const StaticBranchInfo *s = ev.info;
  if (g_log_main_only && !s->in_main) return;
  if (ev.seq < g_log_start || g_logged >= g_log_count) return;
  g_logged++;

  g_log << ev.seq << ','
        << Hex(s->pc) << ','
        << s->size << ','
        << BranchTypeName(s->type) << ','
        << (ev.taken ? 1 : 0) << ','
        << Hex(ev.next_pc) << ','
        << (s->fallthrough   ? Hex(s->fallthrough)   : string("")) << ','
        << (s->direct_target ? Hex(s->direct_target) : string("")) << ','
        << ev.inst_count << ','
        << Hex(p.next_pc) << ','
        << (s->type == BR_COND ? (p.dir_taken ? "1" : "0") : "") << ','
        << (p.btb_hit ? 1 : 0) << ','
        << (wrong ? 0 : 1) << ','
        << s->image << ','
        << (s->in_main ? 1 : 0) << ','
        << '"' << s->routine << "\","
        << '"' << s->disasm << "\"\n";
}

static void ProcessEvent(const BranchEvent &ev)
{
  const StaticBranchInfo *s = ev.info;

  g_count[s->type]++;
  if (s->in_main) g_count_main[s->type]++;
  if (s->type == BR_COND && ev.taken) {
    g_cond_taken++;
    if (s->in_main) g_cond_taken_main++;
  }

  /* Predict from the address alone, score it, then train with the outcome. */
  Prediction p = g_pred->Predict(s->pc, s->pc + s->size);
  bool wrong = g_stats.Record(ev, p);
  if (g_log_on) LogEvent(ev, p, wrong);
  g_pred->Update(ev, p);
}

/* ========================================================================== */
/* Analysis routines (run every time the instrumented code executes)          */
/* ========================================================================== */

/* Runs at the start of every basic block, before any other analysis call in
   that block, so the count is exact when the block's branch event fires.
   (HW2 used IPOINT_ANYWHERE, which lets Pin place the update after it.) */
static VOID PIN_FAST_ANALYSIS_CALL CountBlock(UINT32 n)
{
  if (g_stopped) return;
  if (g_max_insts != 0 && g_insts + n > g_max_insts) {
    g_stopped = true;
    return;
  }
  g_insts += n;
}

static VOID OnBranch(StaticBranchInfo *info, BOOL taken, ADDRINT target)
{
  if (g_stopped) return;

  BranchEvent ev;
  ev.seq        = g_events++;
  ev.inst_count = g_insts;
  ev.info       = info;
  ev.taken      = (taken != 0);
  /* IARG_BRANCH_TARGET_ADDR is the taken target even when a conditional
     branch falls through, so pick the next PC from the outcome. */
  ev.next_pc    = ev.taken ? (UINT64)target : info->pc + info->size;

  ProcessEvent(ev);

  if (g_max_events != 0 && g_events >= g_max_events) g_stopped = true;
}

static VOID OnOther()
{
  if (!g_stopped) g_other++;
}

/* ========================================================================== */
/* Instrumentation (runs once per newly translated piece of code)             */
/* ========================================================================== */

/* Returns false for control flow the predictor does not handle. */
static bool Classify(INS ins, BranchType *type)
{
  if (INS_IsXbegin(ins) || INS_IsXend(ins) ||
      INS_IsFarCall(ins) || INS_IsFarJump(ins) || INS_IsFarRet(ins) ||
      INS_IsSysret(ins) || INS_IsInterrupt(ins)) {
    return false;
  }
  if (INS_IsRet(ins)) {
    *type = BR_RET;
    return true;
  }
  if (INS_IsCall(ins)) {
    *type = INS_IsDirectControlFlow(ins) ? BR_CALL_DIRECT : BR_CALL_INDIRECT;
    return true;
  }
  if (INS_Category(ins) == XED_CATEGORY_COND_BR) {
    *type = BR_COND;
    return true;
  }
  if (INS_IsBranch(ins)) {
    *type = INS_IsDirectControlFlow(ins) ? BR_JMP_DIRECT : BR_JMP_INDIRECT;
    return true;
  }
  return false;
}

static StaticBranchInfo *GetStaticInfo(INS ins, BranchType type)
{
  ADDRINT pc = INS_Address(ins);
  std::map<ADDRINT, StaticBranchInfo *>::iterator it = g_static.find(pc);
  if (it != g_static.end()) return it->second;

  StaticBranchInfo *s = new StaticBranchInfo;
  s->pc            = pc;
  s->size          = INS_Size(ins);
  s->type          = type;
  s->direct_target = INS_IsDirectControlFlow(ins) ? INS_DirectControlFlowTargetAddress(ins) : 0;
  s->fallthrough   = (type == BR_COND || type == BR_CALL_DIRECT || type == BR_CALL_INDIRECT)
                     ? pc + s->size : 0;

  IMG img          = IMG_FindByAddress(pc);
  s->in_main       = IMG_Valid(img) && IMG_IsMainExecutable(img);
  s->image         = IMG_Valid(img) ? BaseName(IMG_Name(img)) : string("?");
  s->routine       = RTN_FindNameByAddress(pc);
  s->disasm        = INS_Disassemble(ins);
  s->id            = g_next_id++;

  g_static[pc] = s;
  return s;
}

static VOID Trace(TRACE trace, VOID *v)
{
  for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
    BBL_InsertCall(bbl, IPOINT_BEFORE, (AFUNPTR)CountBlock,
                   IARG_FAST_ANALYSIS_CALL,
                   IARG_CALL_ORDER, CALL_ORDER_FIRST,
                   IARG_UINT32, BBL_NumIns(bbl),
                   IARG_END);

    for (INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins)) {
      if (!INS_IsControlFlow(ins)) continue;

      BranchType type;
      if (!Classify(ins, &type)) {
        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)OnOther, IARG_END);
        continue;
      }
      StaticBranchInfo *info = GetStaticInfo(ins, type);
      INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)OnBranch,
                     IARG_PTR, info,
                     IARG_BRANCH_TAKEN,
                     IARG_BRANCH_TARGET_ADDR,
                     IARG_END);
    }
  }
}

/* ========================================================================== */
/* Start-up and end-of-run reporting                                          */
/* ========================================================================== */

static VOID ThreadStart(THREADID tid, CONTEXT *ctxt, INT32 flags, VOID *v)
{
  if (tid != 0 && !g_multithreaded) {
    g_multithreaded = true;
    std::cerr << "bpred: WARNING: the program started a second thread; "
                 "this tool supports single-threaded programs only" << std::endl;
  }
}

static void PrintRow(std::ostream &os, const string &name, UINT64 all, UINT64 main_exe)
{
  os << "  " << std::left << std::setw(16) << name << std::right
     << std::setw(14) << all << std::setw(14) << main_exe << "\n";
}

static void PrintSummary(std::ostream &os)
{
  UINT64 total = 0, total_main = 0;
  for (int t = 0; t < BR_NUM_TYPES; t++) {
    total      += g_count[t];
    total_main += g_count_main[t];
  }

  os << "bpred summary\n";
  os << "  Instructions executed: " << g_insts
     << (g_stopped ? "  (stopped at limit)" : "") << "\n";
  os << "  Control-flow events:   " << g_events << "\n";
  if (g_multithreaded) {
    os << "  WARNING: more than one thread ran; results are not valid\n";
  }
  os << "\n";
  os << "  " << std::left << std::setw(16) << "type" << std::right
     << std::setw(14) << "all code" << std::setw(14) << "main exe" << "\n";
  for (int t = 0; t < BR_NUM_TYPES; t++) {
    PrintRow(os, BranchTypeName(t), g_count[t], g_count_main[t]);
    if (t == BR_COND) PrintRow(os, "  cond taken", g_cond_taken, g_cond_taken_main);
  }
  PrintRow(os, "total", total, total_main);
  os << "\n  Other control flow, not predicted: " << g_other << "\n";
}

static void PrintReport(std::ostream &os)
{
  PrintSummary(os);
  os << "\n";
  g_pred->PrintConfig(os);
  os << "\n";
  g_stats.Print(os, g_insts);
  os << "\n";
  g_pred->PrintDiagnostics(os);
  if (KnobTop.Value() > 0) {
    os << "\n";
    g_stats.PrintProfile(os, KnobTop.Value());
  }
}

static VOID Fini(INT32 code, VOID *v)
{
  if (g_log_on) g_log.close();

  std::ofstream out(KnobOutput.Value().c_str());
  PrintReport(out);
  out.close();

  std::cerr << "\n";
  PrintReport(std::cerr);
}

static INT32 Usage()
{
  std::cerr << "bpred: branch prediction simulator for HW3\n\n"
            << KNOB_BASE::StringKnobSummary() << std::endl;
  return -1;
}

int main(int argc, char *argv[])
{
  PIN_InitSymbols();            /* needed for function names in the log */
  if (PIN_Init(argc, argv)) return Usage();
  PIN_SetSyntaxATT();           /* disassembly in the log matches objdump */

  g_max_insts     = KnobMaxInsts.Value();
  g_max_events    = KnobMaxEvents.Value();
  g_log_start     = KnobLogStart.Value();
  g_log_count     = KnobLogCount.Value();
  g_log_main_only = KnobLogMainOnly.Value();

  if (KnobTop.Value() > 0) g_stats.EnableProfile();

  g_pred = CreatePredictor(KnobPredictor.Value());
  if (g_pred == 0) {
    std::cerr << "bpred: unknown predictor '" << KnobPredictor.Value()
              << "'. Available: " << PredictorNames() << std::endl;
    return 1;
  }

  if (!KnobLog.Value().empty()) {
    g_log.open(KnobLog.Value().c_str());
    g_log << "seq,pc,size,type,taken,next_pc,fallthrough_pc,direct_target,"
             "inst_count,pred_next_pc,pred_taken,btb_hit,correct,image,in_main,routine,disasm\n";
    g_log_on = true;
  }

  TRACE_AddInstrumentFunction(Trace, 0);
  PIN_AddThreadStartFunction(ThreadStart, 0);
  PIN_AddFiniFunction(Fini, 0);

  PIN_StartProgram();           /* never returns */
  return 0;
}
