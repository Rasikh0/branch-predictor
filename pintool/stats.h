/*
 * stats.h -- misprediction statistics (Sections 4.2 and 7).
 *
 * Two measures, as defined in report/design_notes.md:
 *
 *   Direction mispredictions: conditional branches whose predicted direction
 *   was wrong. Measures the direction predictor alone.
 *
 *   Wrong next PC: control-flow instructions whose predicted next address was
 *   wrong. Each is charged to exactly one cause, so the causes sum to the total:
 *     return    - any mispredicted return
 *     direction - a conditional branch with the wrong direction
 *     indirect  - an indirect jump/call that hit in the BTB with the wrong target
 *     btb       - everything else: a taken branch that missed the BTB, or a
 *                 direct branch whose stored target was wrong
 *
 *   MPKI = count * 1000 / instructions
 *   CPI_est = 1 + 15 * (wrong-next-PC MPKI) / 1000
 *
 * Optional per-branch profile (EnableProfile): counts per static branch, to
 * list the branches responsible for most mispredictions.
 */
#ifndef HW3_STATS_H
#define HW3_STATS_H

#include <stdint.h>
#include <algorithm>
#include <iomanip>
#include <ostream>
#include <string>
#include <vector>

#include "branch_event.h"
#include "predictor.h"

class Stats {
 public:
  enum Cause { CAUSE_DIRECTION, CAUSE_BTB, CAUSE_INDIRECT, CAUSE_RETURN, NUM_CAUSES };
  static const unsigned kPenaltyCycles = 15;

  Stats() : cond_(0), dir_wrong_(0), wrong_(0), wrong_main_(0), cross_region_(0),
            btb_alias_(0), profile_(false)
  {
    for (int t = 0; t < BR_NUM_TYPES; t++) execs_[t] = wrong_by_type_[t] = 0;
    for (int c = 0; c < NUM_CAUSES; c++) cause_[c] = 0;
  }

  void EnableProfile() { profile_ = true; }

  /* Returns true if the predicted next PC was wrong. */
  bool Record(const BranchEvent &ev, const Prediction &p)
  {
    const StaticBranchInfo *s = ev.info;
    execs_[s->type]++;

    bool dir_wrong = false;
    if (s->type == BR_COND) {
      cond_++;
      dir_wrong = (p.dir_taken != ev.taken);
      if (dir_wrong) dir_wrong_++;
    }
    if (p.btb_hit && p.btb_alias) btb_alias_++;

    bool wrong = (p.next_pc != ev.next_pc);
    if (profile_) {
      if (s->id >= per_.size()) per_.resize(s->id + 1024);
      BranchProfile &b = per_[s->id];
      b.info = s;
      b.execs++;
      if (ev.taken) b.taken++;
      if (dir_wrong) b.dir_wrong++;
      if (wrong) b.wrong++;
    }
    if (!wrong) return false;

    wrong_++;
    wrong_by_type_[s->type]++;
    if (s->in_main) wrong_main_++;
    /* Low 32 bits right, upper bits wrong: lost only to 32-bit storage. */
    if (((p.next_pc ^ ev.next_pc) & 0xffffffffULL) == 0) cross_region_++;

    if (s->type == BR_RET) cause_[CAUSE_RETURN]++;
    else if (dir_wrong) cause_[CAUSE_DIRECTION]++;
    else if ((s->type == BR_JMP_INDIRECT || s->type == BR_CALL_INDIRECT) && p.btb_hit)
      cause_[CAUSE_INDIRECT]++;
    else cause_[CAUSE_BTB]++;
    return true;
  }

  uint64_t DirectionWrong() const   { return dir_wrong_; }
  uint64_t WrongNextPc() const      { return wrong_; }
  uint64_t Cause(int c) const       { return cause_[c]; }
  uint64_t WrongByType(int t) const { return wrong_by_type_[t]; }
  uint64_t Execs(int t) const       { return execs_[t]; }
  uint64_t CrossRegion() const      { return cross_region_; }
  uint64_t BtbAlias() const         { return btb_alias_; }

  static double Mpki(uint64_t count, uint64_t insts)
  {
    return insts ? count * 1000.0 / insts : 0.0;
  }

  double EstimatedCpi(uint64_t insts) const
  {
    return 1.0 + kPenaltyCycles * Mpki(wrong_, insts) / 1000.0;
  }

  void Print(std::ostream &os, uint64_t insts) const
  {
    os << "Prediction results\n";
    os << "  " << std::left << std::setw(40) << "metric" << std::right << std::setw(12)
       << "count" << std::setw(12) << "MPKI" << "\n";
    Line(os, "direction_mispredictions", dir_wrong_, insts);
    Line(os, "wrong_next_pc_total", wrong_, insts);
    Line(os, "  cause_direction", cause_[CAUSE_DIRECTION], insts);
    Line(os, "  cause_btb_target", cause_[CAUSE_BTB], insts);
    Line(os, "  cause_indirect_target", cause_[CAUSE_INDIRECT], insts);
    Line(os, "  cause_return", cause_[CAUSE_RETURN], insts);
    Line(os, "  in_main_executable", wrong_main_, insts);
    Line(os, "  cross_region_only", cross_region_, insts);
    Line(os, "btb_alias_hits", btb_alias_, insts);
    os << "  " << std::left << std::setw(40) << "conditional_branches" << std::right
       << std::setw(12) << cond_ << "\n";
    os << "  " << std::left << std::setw(40) << "direction_accuracy_percent" << std::right
       << std::setw(12) << std::fixed << std::setprecision(2)
       << (cond_ ? 100.0 * (cond_ - dir_wrong_) / cond_ : 0.0) << "\n";
    os << "  " << std::left << std::setw(40) << "estimated_cpi" << std::right
       << std::setw(12) << std::fixed << std::setprecision(4) << EstimatedCpi(insts) << "\n";
    os.unsetf(std::ios::floatfield);

    os << "\n  Wrong next PC by type\n";
    os << "  " << std::left << std::setw(16) << "type" << std::right << std::setw(12)
       << "executed" << std::setw(12) << "wrong" << std::setw(10) << "% wrong" << "\n";
    for (int t = 0; t < BR_NUM_TYPES; t++) {
      os << "  " << std::left << std::setw(16) << BranchTypeName(t) << std::right
         << std::setw(12) << execs_[t] << std::setw(12) << wrong_by_type_[t]
         << std::setw(10) << std::fixed << std::setprecision(2)
         << (execs_[t] ? 100.0 * wrong_by_type_[t] / execs_[t] : 0.0) << "\n";
    }
    os.unsetf(std::ios::floatfield);
  }

  /* The n static branches with the most wrong next PCs. */
  void PrintProfile(std::ostream &os, unsigned n) const
  {
    std::vector<const BranchProfile *> v;
    for (size_t i = 0; i < per_.size(); i++)
      if (per_[i].info && per_[i].wrong) v.push_back(&per_[i]);
    std::sort(v.begin(), v.end(), [](const BranchProfile *a, const BranchProfile *b) {
      return a->wrong > b->wrong;
    });
    if (v.size() > n) v.resize(n);

    os << "Top " << v.size() << " branches by wrong next PC (of " << wrong_ << " in total)\n";
    os << "  wrong% = wrong / executed. bias% (conditional branches) = how often the\n"
       << "  branch goes its less common way, i.e. the error of always guessing its\n"
       << "  common direction; wrong% near bias% means history isn't helping.\n";
    os << "  " << std::right << std::setw(3) << "#" << "  " << std::left << std::setw(18)
       << "pc" << std::setw(14) << "type" << std::right << std::setw(11) << "executed"
       << std::setw(8) << "taken%" << std::setw(11) << "dir wrong" << std::setw(11) << "wrong"
       << std::setw(8) << "wrong%" << std::setw(7) << "bias%" << std::setw(7) << "%all"
       << std::setw(7) << "cum%" << "  " << std::left << "image:function" << "\n";
    double cum = 0;
    for (size_t i = 0; i < v.size(); i++) {
      const BranchProfile &b = *v[i];
      double share = wrong_ ? 100.0 * b.wrong / wrong_ : 0.0;
      cum += share;
      std::string where = b.info->image + ":" +
                          (b.info->routine.empty() ? std::string("?") : b.info->routine);
      if (where.size() > 40) where = where.substr(0, 40);
      os << "  " << std::right << std::setw(3) << (i + 1) << "  " << std::left
         << std::setw(18) << Hex(b.info->pc) << std::setw(14) << BranchTypeName(b.info->type)
         << std::right << std::setw(11) << b.execs << std::fixed << std::setprecision(1)
         << std::setw(8) << (b.execs ? 100.0 * b.taken / b.execs : 0.0)
         << std::setw(11) << b.dir_wrong << std::setw(11) << b.wrong
         << std::setw(8) << (b.execs ? 100.0 * b.wrong / b.execs : 0.0);
      if (b.info->type == BR_COND && b.execs) {
        uint64_t minority = std::min(b.taken, b.execs - b.taken);
        os << std::setw(7) << 100.0 * minority / b.execs;
      } else {
        os << std::setw(7) << "-";
      }
      os << std::setw(7) << share << std::setw(7) << cum << "  " << std::left << where
         << "\n";
      os.unsetf(std::ios::floatfield);
    }
  }

 private:
  struct BranchProfile {
    const StaticBranchInfo *info = 0;
    uint64_t execs = 0, taken = 0, dir_wrong = 0, wrong = 0;
  };

  static std::string Hex(uint64_t v)
  {
    static const char *d = "0123456789abcdef";
    std::string s;
    do { s.insert(s.begin(), d[v & 15]); v >>= 4; } while (v);
    return "0x" + s;
  }

  static void Line(std::ostream &os, const char *name, uint64_t count, uint64_t insts)
  {
    os << "  " << std::left << std::setw(40) << name << std::right << std::setw(12) << count
       << std::setw(12) << std::fixed << std::setprecision(4) << Mpki(count, insts) << "\n";
    os.unsetf(std::ios::floatfield);
  }

  uint64_t execs_[BR_NUM_TYPES], wrong_by_type_[BR_NUM_TYPES];
  uint64_t cause_[NUM_CAUSES];
  uint64_t cond_, dir_wrong_, wrong_, wrong_main_, cross_region_, btb_alias_;
  bool profile_;
  std::vector<BranchProfile> per_;
};

#endif
