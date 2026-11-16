/*
 * tournament.h -- tournament direction predictor (Section 4 baseline).
 *
 *   Local:  PC -> local history table (LHT) -> local pattern table (LPT)
 *   Global: global history register (GHR) -> global pattern table (GPT)
 *   Chooser: GHR -> choice table; picks local or global
 *
 * Baseline sizes (the defaults):
 *   LHT    1024 x 10-bit histories   10,240 bits
 *   LPT    1024 x 3-bit counters      3,072
 *   GPT    4096 x 2-bit counters      8,192
 *   Choice 4096 x 2-bit counters      8,192
 *   GHR    12 bits                       12
 *   Total                            29,708
 *
 * Choices (see report/design_notes.md):
 *   - LHT indexed by the low bits of the PC (no alignment shift on x86).
 *   - GPT and chooser indexed by the GHR alone, as the spec describes.
 *     Improvement 1 (gshare) instead indexes the GPT with PC XOR history,
 *     optionally with a longer history and a bigger GPT; the chooser keeps
 *     using the most recent outcomes in the GHR.
 *   - The GHR records conditional branch outcomes only.
 *   - Chooser trained only when local and global disagree.
 *   - Counters start weakly not-taken; the chooser starts weakly local.
 */
#ifndef HW3_TOURNAMENT_H
#define HW3_TOURNAMENT_H

#include <stdint.h>
#include <iomanip>
#include <ostream>
#include <vector>

#include "predictor.h"

class TournamentPredictor : public DirectionPredictor {
 public:
  /* global_index_bits / choice_index_bits: log2 of the GPT / choice table
     sizes; 0 means "same as global_hist_bits" (the baseline). */
  TournamentPredictor(unsigned lht_entries = 1024, unsigned local_hist_bits = 10,
                      unsigned local_ctr_bits = 3, unsigned global_hist_bits = 12,
                      unsigned global_ctr_bits = 2, unsigned choice_ctr_bits = 2,
                      unsigned global_index_bits = 0, unsigned choice_index_bits = 0,
                      bool global_pc_hash = false)
    : lht_entries_(lht_entries), local_hist_bits_(local_hist_bits),
      local_ctr_bits_(local_ctr_bits), global_hist_bits_(global_hist_bits),
      global_ctr_bits_(global_ctr_bits), choice_ctr_bits_(choice_ctr_bits),
      global_index_bits_(global_index_bits ? global_index_bits : global_hist_bits),
      choice_index_bits_(choice_index_bits ? choice_index_bits : global_hist_bits),
      global_pc_hash_(global_pc_hash),
      lht_(lht_entries, 0),
      lpt_(1u << local_hist_bits, bp::SatWeakNotTaken(local_ctr_bits)),
      gpt_(1u << global_index_bits_, bp::SatWeakNotTaken(global_ctr_bits)),
      choice_(1u << choice_index_bits_, bp::SatWeakNotTaken(choice_ctr_bits)),
      ghr_(0) {}

  /* Final direction prediction for the branch at pc. No side effects. */
  bool Predict(uint64_t pc) const override { return Lookup(pc).final_taken; }

  /* The global history register (shared with the indirect target predictor). */
  uint64_t History() const override { return ghr_; }

  /* The global component's own prediction (for tests and analysis). */
  bool PredictGlobal(uint64_t pc) const { return Lookup(pc).global_taken; }

  /* Train with the outcome of the conditional branch at pc. */
  void Update(uint64_t pc, bool taken) override
  {
    Parts p = Lookup(pc);

    if (p.local_taken != p.global_taken) {
      /* Move the chooser toward whichever component was right. */
      choice_[p.c_idx] = bp::SatUpdate(choice_[p.c_idx], p.global_taken == taken,
                                       choice_ctr_bits_);
    }
    lpt_[p.lpt_idx] = bp::SatUpdate(lpt_[p.lpt_idx], taken, local_ctr_bits_);
    gpt_[p.g_idx]   = bp::SatUpdate(gpt_[p.g_idx], taken, global_ctr_bits_);

    lht_[p.lht_idx] = (uint16_t)(((lht_[p.lht_idx] << 1) | (taken ? 1 : 0))
                                 & bp::LowMask(local_hist_bits_));
    ghr_ = (uint32_t)(((ghr_ << 1) | (taken ? 1 : 0)) & bp::LowMask(global_hist_bits_));
  }

  uint64_t StorageBits() const override
  {
    return LhtBits() + LptBits() + GptBits() + ChoiceBits() + global_hist_bits_;
  }

  void PrintConfig(std::ostream &os) const override
  {
    Row(os, "Local history table", lht_entries_, local_hist_bits_, LhtBits());
    Row(os, "Local pattern table", lpt_.size(), local_ctr_bits_, LptBits());
    Row(os, global_pc_hash_ ? "Global pattern table (gshare)" : "Global pattern table",
        gpt_.size(), global_ctr_bits_, GptBits());
    Row(os, "Choice table", choice_.size(), choice_ctr_bits_, ChoiceBits());
    Row(os, "Global history register", 1, global_hist_bits_, global_hist_bits_);
    os << "  " << std::left << std::setw(34) << "Direction predictor total"
       << std::right << std::setw(10) << StorageBits() << "\n";
  }

 private:
  struct Parts {
    bool local_taken, global_taken, use_global, final_taken;
    unsigned lht_idx, lpt_idx, g_idx, c_idx;
  };

  Parts Lookup(uint64_t pc) const
  {
    Parts p;
    p.lht_idx      = (unsigned)(pc & (lht_entries_ - 1));
    p.lpt_idx      = lht_[p.lht_idx];
    /* Baseline: GPT index = GHR. gshare: GHR XOR PC (history folded first
       if it is longer than the index). */
    uint64_t h     = Fold(ghr_, global_hist_bits_, global_index_bits_);
    p.g_idx        = (unsigned)((global_pc_hash_ ? (h ^ pc) : h)
                                & bp::LowMask(global_index_bits_));
    p.c_idx        = (unsigned)(ghr_ & bp::LowMask(choice_index_bits_));
    p.local_taken  = bp::SatTaken(lpt_[p.lpt_idx], local_ctr_bits_);
    p.global_taken = bp::SatTaken(gpt_[p.g_idx], global_ctr_bits_);
    p.use_global   = bp::SatTaken(choice_[p.c_idx], choice_ctr_bits_);
    p.final_taken  = p.use_global ? p.global_taken : p.local_taken;
    return p;
  }

  /* XOR-fold a `from`-bit value down to `to` bits. */
  static uint64_t Fold(uint64_t v, unsigned from, unsigned to)
  {
    if (from <= to) return v;
    uint64_t out = 0;
    for (unsigned done = 0; done < from; done += to) out ^= (v >> done) & bp::LowMask(to);
    return out;
  }

  uint64_t LhtBits() const    { return (uint64_t)lht_entries_ * local_hist_bits_; }
  uint64_t LptBits() const    { return (uint64_t)lpt_.size() * local_ctr_bits_; }
  uint64_t GptBits() const    { return (uint64_t)gpt_.size() * global_ctr_bits_; }
  uint64_t ChoiceBits() const { return (uint64_t)choice_.size() * choice_ctr_bits_; }

  static void Row(std::ostream &os, const char *name, uint64_t n, unsigned w, uint64_t bits)
  {
    os << "  " << std::left << std::setw(34) << name << std::right << std::setw(10) << bits
       << "   (" << n << " x " << w << ")\n";
  }

  unsigned lht_entries_, local_hist_bits_, local_ctr_bits_;
  unsigned global_hist_bits_, global_ctr_bits_, choice_ctr_bits_;
  unsigned global_index_bits_, choice_index_bits_;
  bool global_pc_hash_;
  std::vector<uint16_t> lht_;
  std::vector<uint8_t>  lpt_, gpt_, choice_;
  uint32_t ghr_;
};

#endif
