/*
 * replay.cpp -- replay a bpred CSV event log through a predictor, without Pin.
 *
 * Usage:  ./replay LOG.csv [predictor] [top]  (predictor defaults to baseline;
 *         top > 0 also lists the top branches by wrong next PC)
 *
 * The log must contain every event from the start of the run, i.e. it was
 * recorded with -log_main 0 and a -log_count large enough for the whole run.
 * (A main-executable-only log skips library branches, so the predictor's
 * history would differ from the original run.)
 *
 * For each event, the replayed prediction is compared with the one the Pin
 * tool logged. Zero mismatches means the log alone reproduces the run.
 */
#include <stdint.h>
#include <stdlib.h>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "frontend.h"
#include "stats.h"

/* Split one CSV line, honoring double quotes (disassembly contains commas). */
static std::vector<std::string> SplitCsv(const std::string &line)
{
  std::vector<std::string> out;
  std::string cur;
  bool quoted = false;
  for (size_t i = 0; i < line.size(); i++) {
    char c = line[i];
    if (c == '"') quoted = !quoted;
    else if (c == ',' && !quoted) { out.push_back(cur); cur.clear(); }
    else cur += c;
  }
  out.push_back(cur);
  return out;
}

static int TypeFromName(const std::string &n)
{
  for (int t = 0; t < BR_NUM_TYPES; t++)
    if (n == BranchTypeName(t)) return t;
  return -1;
}

static uint64_t Num(const std::string &s) { return strtoull(s.c_str(), 0, 0); }

int main(int argc, char **argv)
{
  if (argc < 2) {
    std::cerr << "usage: " << argv[0] << " LOG.csv [predictor] [top]\n";
    return 2;
  }
  std::ifstream in(argv[1]);
  if (!in) {
    std::cerr << "cannot open " << argv[1] << "\n";
    return 2;
  }
  std::string name = argc > 2 ? argv[2] : "baseline";
  unsigned top = argc > 3 ? (unsigned)atoi(argv[3]) : 0;
  FrontEndPredictor *pred = CreatePredictor(name);
  if (!pred) {
    std::cerr << "unknown predictor '" << name << "'. Available: " << PredictorNames() << "\n";
    return 2;
  }

  std::string line;
  std::getline(in, line);
  std::vector<std::string> header = SplitCsv(line);
  std::map<std::string, size_t> col;
  for (size_t i = 0; i < header.size(); i++) col[header[i]] = i;
  const char *needed[] = {"seq", "pc", "size", "type", "taken", "next_pc", "inst_count", "image"};
  for (const char *n : needed) {
    if (!col.count(n)) {
      std::cerr << "log is missing column '" << n << "'\n";
      return 2;
    }
  }
  bool has_pred = col.count("pred_next_pc") > 0;
  bool has_in_main = col.count("in_main") > 0;

  std::map<uint64_t, StaticBranchInfo> statics;
  uint32_t next_id = 0;
  std::string main_image;
  Stats stats;
  if (top > 0) stats.EnableProfile();
  uint64_t events = 0, mismatches = 0, insts = 0, expected_seq = 0;

  while (std::getline(in, line)) {
    if (line.empty()) continue;
    std::vector<std::string> f = SplitCsv(line);
    if (f.size() < header.size()) {
      std::cerr << "malformed line: " << line << "\n";
      return 2;
    }
    uint64_t seq = Num(f[col["seq"]]);
    if (seq != expected_seq) {
      std::cerr << "event " << expected_seq << " is missing (found " << seq
                << "). Record the log with -log_main 0 and a large enough -log_count.\n";
      return 2;
    }
    expected_seq++;

    uint64_t pc = Num(f[col["pc"]]);
    StaticBranchInfo &s = statics[pc];
    if (s.size == 0) {
      int t = TypeFromName(f[col["type"]]);
      if (t < 0) {
        std::cerr << "unknown type in line: " << line << "\n";
        return 2;
      }
      s.pc = pc;
      s.size = (uint32_t)Num(f[col["size"]]);
      s.type = (BranchType)t;
      s.fallthrough = (t == BR_COND || t == BR_CALL_DIRECT || t == BR_CALL_INDIRECT)
                      ? pc + s.size : 0;
      s.direct_target = 0;
      s.image = f[col["image"]];
      s.id = next_id++;
      if (col.count("routine")) s.routine = f[col["routine"]];
      if (has_in_main) {
        s.in_main = Num(f[col["in_main"]]) != 0;
      } else {
        /* Older logs: guess from the name. Libraries have ".so" in theirs. */
        s.in_main = s.image.find(".so") == std::string::npos && s.image != "?";
      }
    }

    BranchEvent ev;
    ev.seq = seq;
    ev.inst_count = Num(f[col["inst_count"]]);
    ev.info = &s;
    ev.taken = Num(f[col["taken"]]) != 0;
    ev.next_pc = Num(f[col["next_pc"]]);
    insts = ev.inst_count;

    Prediction p = pred->Predict(pc, pc + s.size);
    if (has_pred && p.next_pc != Num(f[col["pred_next_pc"]])) {
      if (mismatches < 10)
        std::cerr << "mismatch at seq " << seq << ": replay predicts 0x" << std::hex
                  << p.next_pc << ", log has " << f[col["pred_next_pc"]] << std::dec << "\n";
      mismatches++;
    }
    stats.Record(ev, p);
    pred->Update(ev, p);
    events++;
  }

  std::cout << "Replayed " << events << " events from " << argv[1] << "\n";
  std::cout << "Instructions (as of the last event): " << insts << "\n\n";
  pred->PrintConfig(std::cout);
  std::cout << "\n";
  stats.Print(std::cout, insts);
  std::cout << "\n";
  pred->PrintDiagnostics(std::cout);
  if (top > 0) {
    std::cout << "\n";
    stats.PrintProfile(std::cout, top);
  }
  if (has_pred) {
    std::cout << "\nPredictions that differ from the log: " << mismatches << "\n";
  }
  delete pred;
  return mismatches ? 1 : 0;
}
