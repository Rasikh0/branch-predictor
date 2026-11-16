/*
 * frontend.h -- a front end built from a tournament direction predictor,
 * a BTB and a RAS. FrontEndConfig's defaults are the Section 4 baseline.
 *
 * Next-PC prediction (see report/design_notes.md):
 *   BTB miss            -> pc + size (the front end doesn't know it's a branch)
 *   BTB hit, cond       -> BTB target if the direction predictor says taken, else pc + size
 *   BTB hit, jump/call  -> BTB target (for indirect: the most recent target)
 *   BTB hit, indirect   -> with the indirect target predictor (improvement 2):
 *                          its target if it hits, else the BTB target
 *   BTB hit, return     -> top of RAS; if the RAS is empty, the BTB target
 *
 * Update, with the actual outcome:
 *   conditional branch  -> train the direction predictor
 *   taken branch        -> refresh or allocate its BTB entry
 *   call                -> push the return address (pc + size) on the RAS
 *   return              -> pop the RAS
 *   indirect jump/call  -> train the indirect target predictor, if present
 */
#ifndef HW3_FRONTEND_H
#define HW3_FRONTEND_H

#include <stdint.h>
#include <iomanip>
#include <ostream>
#include <string>

#include "btb.h"
#include "itp.h"
#include "predictor.h"
#include "ras.h"
#include "tage.h"
#include "tournament.h"
#include "unified.h"

struct FrontEndConfig {
  std::string name = "baseline";
  /* Direction predictor */
  unsigned lht_entries      = 1024;
  unsigned local_hist_bits  = 10;
  unsigned local_ctr_bits   = 3;
  unsigned global_hist_bits = 12;
  unsigned global_ctr_bits  = 2;
  unsigned choice_ctr_bits  = 2;
  unsigned global_index_bits = 0;      /* 0: same as global_hist_bits */
  unsigned choice_index_bits = 0;      /* 0: same as global_hist_bits */
  bool     global_pc_hash    = false;  /* improvement 1: gshare-style index */
  /* BTB */
  unsigned btb_entries      = 256;
  unsigned btb_ways         = 1;
  unsigned btb_tag_bits     = 16;
  unsigned btb_target_bits  = 32;
  /* RAS */
  unsigned ras_entries      = 16;
  unsigned ras_entry_bits   = 32;
  /* Indirect target predictor (improvement 2); 0 entries = none */
  unsigned itp_entries      = 0;
  unsigned itp_tag_bits     = 8;
  unsigned itp_target_bits  = 32;
  unsigned itp_hist_bits    = 16;
  unsigned itp_ghr_bits     = 0;   /* GHR bits in the context (itp4_gh) */
  unsigned itp_ways         = 1;
  bool     itp_fold         = false;
  /* Section 6 option C: unified target table replacing BTB + ITP; 0 = none */
  int      uni_mode         = 0;     /* UnifiedTargetTable::HIST_ALL or TWO_PROBE */
  unsigned uni_sets         = 128;
  unsigned uni_ways         = 5;
  unsigned uni_ghr_bits     = 8;
  /* Section 9 bonus: TAGE-SC-L instead of the tournament predictor */
  bool       use_tage = false;
  TageConfig tage;
};

class FrontEnd : public FrontEndPredictor {
 public:
  explicit FrontEnd(const FrontEndConfig &c)
    : cfg_(c),
      dir_(MakeDirection(c)),
      btb_(c.btb_entries, c.btb_ways, c.btb_tag_bits, c.btb_target_bits),
      ras_(c.ras_entries, c.ras_entry_bits),
      itp_(c.itp_entries ? c.itp_entries : 1, c.itp_tag_bits, c.itp_target_bits,
           c.itp_hist_bits, c.itp_ghr_bits, c.itp_entries ? c.itp_ways : 1, c.itp_fold),
      has_itp_(c.itp_entries != 0),
      uni_(c.uni_mode ? c.uni_mode : UnifiedTargetTable::HIST_ALL,
           c.uni_mode ? c.uni_sets : 1, c.uni_mode ? c.uni_ways : 1, 16, 32, 14, c.uni_ghr_bits),
      uni_mode_(c.uni_mode) {}

  ~FrontEnd() { delete dir_; }
  FrontEnd(const FrontEnd &) = delete;
  FrontEnd &operator=(const FrontEnd &) = delete;

  const char *Name() const { return cfg_.name.c_str(); }

  Prediction Predict(uint64_t pc, uint64_t seq_next)
  {
    Prediction p;
    p.dir_taken = dir_->Predict(pc);

    if (uni_mode_) return PredictUnified(pc, seq_next, p);

    BtbResult b = btb_.Lookup(pc);
    p.btb_hit   = b.hit;
    p.btb_type  = b.type;
    p.btb_alias = b.alias;

    if (!b.hit) {
      p.next_pc = seq_next;
    } else if (b.type == BR_COND) {
      p.next_pc = p.dir_taken ? b.target : seq_next;
    } else if (b.type == BR_RET) {
      uint64_t addr;
      if (ras_.Top(pc, &addr)) {
        p.next_pc  = addr;
        p.from_ras = true;
      } else {
        p.next_pc = b.target;  /* RAS empty: fall back to the BTB */
      }
    } else if (has_itp_ && (b.type == BR_JMP_INDIRECT || b.type == BR_CALL_INDIRECT)) {
      uint64_t t;
      p.next_pc = itp_.Lookup(pc, dir_->History(), &t) ? t : b.target;
    } else {
      p.next_pc = b.target;
    }
    return p;
  }

  void Update(const BranchEvent &ev, const Prediction &p)
  {
    (void)p;
    const StaticBranchInfo *s = ev.info;
    uint64_t ghr = dir_->History();   /* as seen at prediction time */
    if (s->type == BR_COND) dir_->Update(s->pc, ev.taken);
    if (uni_mode_) {
      uni_.Update(s->pc, ghr, ev.next_pc, s->type, ev.taken);
      if (s->type == BR_CALL_DIRECT || s->type == BR_CALL_INDIRECT) ras_.Push(s->fallthrough);
      if (s->type == BR_RET) ras_.Pop();
      return;
    }
    if (ev.taken) btb_.Update(s->pc, ev.next_pc, s->type);
    if (s->type == BR_CALL_DIRECT || s->type == BR_CALL_INDIRECT) ras_.Push(s->fallthrough);
    if (s->type == BR_RET) ras_.Pop();
    if (has_itp_ && (s->type == BR_JMP_INDIRECT || s->type == BR_CALL_INDIRECT))
      itp_.Update(s->pc, dir_->History(), ev.next_pc);
  }

  uint64_t StorageBits() const
  {
    if (uni_mode_) return dir_->StorageBits() + uni_.StorageBits() + ras_.StorageBits();
    return dir_->StorageBits() + btb_.StorageBits() + ras_.StorageBits() +
           (has_itp_ ? itp_.StorageBits() : 0);
  }

  void PrintConfig(std::ostream &os) const
  {
    os << "Predictor: " << Name() << "\n";
    dir_->PrintConfig(os);
    if (uni_mode_) {
      uni_.PrintConfig(os);
    } else {
      btb_.PrintConfig(os);
    }
    ras_.PrintConfig(os);
    if (has_itp_ && !uni_mode_) itp_.PrintConfig(os);
    os << "  " << std::left << std::setw(34) << "Total" << std::right << std::setw(10)
       << StorageBits() << "   (budget " << kStorageBudgetBits << ": "
       << (StorageBits() <= kStorageBudgetBits ? "within" : "OVER") << ")\n";
  }

  void PrintDiagnostics(std::ostream &os) const
  {
    dir_->PrintDiagnostics(os);
    ras_.PrintDiagnostics(os);
    if (has_itp_ && !uni_mode_) itp_.PrintDiagnostics(os);
    if (uni_mode_) uni_.PrintDiagnostics(os);
  }

 private:
  static DirectionPredictor *MakeDirection(const FrontEndConfig &c)
  {
    if (c.use_tage) return new TagePredictor(c.tage);
    return new TournamentPredictor(c.lht_entries, c.local_hist_bits, c.local_ctr_bits,
                                   c.global_hist_bits, c.global_ctr_bits, c.choice_ctr_bits,
                                   c.global_index_bits, c.choice_index_bits, c.global_pc_hash);
  }

  /* Same next-PC rules as the BTB path, with the unified table's entry. */
  Prediction PredictUnified(uint64_t pc, uint64_t seq_next, Prediction p)
  {
    UnifiedTargetTable::Result r = uni_.Lookup(pc, dir_->History());
    p.btb_hit   = r.hit;
    p.btb_type  = r.type;
    p.btb_alias = r.alias;
    if (!r.hit) {
      p.next_pc = seq_next;
    } else if (r.type == BR_COND) {
      p.next_pc = p.dir_taken ? r.target : seq_next;
    } else if (r.type == BR_RET) {
      uint64_t addr;
      if (ras_.Top(pc, &addr)) {
        p.next_pc  = addr;
        p.from_ras = true;
      } else {
        p.next_pc = r.target;
      }
    } else {
      p.next_pc = r.target;
    }
    return p;
  }

 public:

 private:
  FrontEndConfig      cfg_;
  DirectionPredictor *dir_;
  Btb                 btb_;
  Ras                 ras_;
  IndirectPredictor   itp_;
  bool                has_itp_;
  UnifiedTargetTable  uni_;
  int                 uni_mode_;
};

/*
 * Predictor configurations selectable with -predictor NAME.
 *
 *   baseline  Section 4 baseline                                  43,537 bits
 *   gshare12  improvement 1, index change only: GPT index = PC XOR
 *             12-bit history, same table sizes                    43,537
 *   gshare    improvement 1: GPT index = PC XOR 14-bit history,
 *             16K-entry GPT                                       68,115
 *   itp       improvement 2: 512-entry direct-mapped indirect target
 *             predictor, context = target history                 64,545
 *   itp4      improvement 2, refined: 128 sets x 4 ways, 14-bit
 *             context hashed into index and tag                   65,567
 *   itp4_gh   itp4, context also holds the 8 most recent
 *             conditional outcomes (the direction predictor's
 *             GHR, so no extra bits)                              65,567
 *   ras64     improvement 3: 64-entry RAS                         45,075
 *   btb4way   improvement 4: 4-way set-associative BTB, LRU       44,049
 *   final_v1  gshare + itp + ras64 + btb4way                      91,173
 *   final     gshare + itp4_gh + ras64 + btb4way                  92,195
 *
 * Section 6, option C (unified target table instead of BTB + ITP; the
 * direction predictor and RAS are final's, so only the target side differs):
 *   uni_hist    every lookup keyed by (PC, PHR + 8 GHR bits),
 *               640 entries                                       91,555
 *   uni_phr     same, keyed by (PC, PHR) only                     91,555
 *   uni_2probe  PC probe, then (PC, history) probe if indirect,
 *               640 entries with a kind bit                       92,195
 *
 * Section 9 bonus, TAGE-SC-L (tage.h). All use final's target side (4-way
 * BTB, itp4_gh, 64-entry RAS) so only the direction predictor differs:
 *   final_tour  final with the baseline tournament predictor      67,617
 *   tage_small  TAGE-SC-L about the tournament's size             67,572
 *   tage_base   TAGE only (no loop predictor, no SC), full size   89,463
 *   tage_l      TAGE + loop predictor                             90,078
 *   tage        TAGE + loop predictor + SC (L-TAGE-SC)            96,252
 * Each improvement changes only its own structure; everything else is the
 * baseline, so it can be measured independently.
 */
static inline void ApplyGshare(FrontEndConfig &c, unsigned hist_bits)
{
  c.global_pc_hash    = true;
  c.global_hist_bits  = hist_bits;
  c.global_index_bits = hist_bits;
  c.choice_index_bits = 12;          /* chooser stays 4096 entries, as in the baseline */
}
static inline void ApplyItp(FrontEndConfig &c, unsigned ways, bool fold, unsigned hist_bits,
                            unsigned ghr_bits)
{
  c.itp_entries   = 512;
  c.itp_ways      = ways;
  c.itp_fold      = fold;
  c.itp_hist_bits = hist_bits;
  c.itp_ghr_bits  = ghr_bits;
}
static inline void ApplyRas64(FrontEndConfig &c)   { c.ras_entries = 64; }
static inline void ApplyBtb4Way(FrontEndConfig &c) { c.btb_ways = 4; }

static inline FrontEndPredictor *CreatePredictor(const std::string &name)
{
  FrontEndConfig c;
  c.name = name;
  if (name == "baseline") {
  } else if (name == "gshare12") {
    ApplyGshare(c, 12);
  } else if (name == "gshare") {
    ApplyGshare(c, 14);
  } else if (name == "itp") {
    ApplyItp(c, 1, false, 16, 0);
  } else if (name == "itp4") {
    ApplyItp(c, 4, true, 14, 0);
  } else if (name == "itp4_gh") {
    ApplyItp(c, 4, true, 14, 8);
  } else if (name == "ras64") {
    ApplyRas64(c);
  } else if (name == "btb4way") {
    ApplyBtb4Way(c);
  } else if (name == "final_v1") {
    ApplyGshare(c, 14);
    ApplyItp(c, 1, false, 16, 0);
    ApplyRas64(c);
    ApplyBtb4Way(c);
  } else if (name == "final") {
    ApplyGshare(c, 14);
    ApplyItp(c, 4, true, 14, 8);
    ApplyRas64(c);
    ApplyBtb4Way(c);
  } else if (name == "final_tour" || name == "tage_small" || name == "tage_base" ||
             name == "tage_l" || name == "tage") {
    ApplyItp(c, 4, true, 14, 8);
    ApplyRas64(c);
    ApplyBtb4Way(c);
    if (name != "final_tour") {
      c.use_tage = true;
      c.tage.use_loop = (name != "tage_base");
      c.tage.use_sc = (name == "tage" || name == "tage_small");
      if (name == "tage_small") {
        c.tage.bim_log = 11;
        c.tage.table_log = 8;
        c.tage.sc_bias_log = 8;
        c.tage.sc_gehl_log = 7;
      }
    }
  } else if (name == "uni_hist" || name == "uni_phr" || name == "uni_2probe") {
    ApplyGshare(c, 14);
    ApplyRas64(c);
    c.uni_mode     = name == "uni_2probe" ? (int)UnifiedTargetTable::TWO_PROBE
                                          : (int)UnifiedTargetTable::HIST_ALL;
    c.uni_ghr_bits = name == "uni_phr" ? 0 : 8;
  } else {
    return 0;
  }
  return new FrontEnd(c);
}

static inline const char *PredictorNames()
{
  return "baseline gshare12 gshare itp itp4 itp4_gh ras64 btb4way final_v1 final "
         "uni_hist uni_phr uni_2probe final_tour tage_small tage_base tage_l tage";
}

#endif
