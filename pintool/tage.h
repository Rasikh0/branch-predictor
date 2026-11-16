/*
 * tage.h -- L-TAGE-SC direction predictor (Section 9, bonus).
 *
 * A simplified TAGE-SC-L after Seznec (CBP 2016), sized to fit the budget
 * alongside the final design's target predictors. Three components:
 *
 * 1. TAGE: a bimodal base table plus N tagged tables indexed with
 *    geometrically increasing global history lengths. The prediction comes
 *    from the matching table with the longest history (the provider); the
 *    next-longest match (or the bimodal table) is the alternate.
 * 2. Loop predictor: recognizes loops with a constant trip count and
 *    predicts their exit, which needs a history longer than any table's.
 * 3. Statistical corrector (SC): small tables of signed counters, indexed by
 *    PC with the prediction so far and by global history, that learn when
 *    TAGE's prediction for a branch tends to be wrong and overturn it.
 *
 * Prediction: TAGE -> loop predictor may override -> SC may override.
 *
 * ---- TAGE ---------------------------------------------------------------
 * Global history: outcomes of conditional branches (as in the baseline), up
 * to the longest length; path history: 16 bits, one address bit per
 * conditional branch.
 * Bimodal: 2^bim_log 2-bit counters, index = PC mod 2^bim_log.
 * Tagged table i (i = 0..N-1), 2^table_log entries of
 *   ctr (3-bit, taken if >= 4) + u (2-bit usefulness) + tag (tag_bits[i])
 *   index = (PC XOR (PC >> (table_log - i)) XOR F_idx(h, L_i) XOR P(path)) mod 2^table_log
 *   tag   = (PC XOR F_tag1(h, L_i) XOR (F_tag2(h, L_i) << 1)) mod 2^tag_bits[i]
 * where F(h, L) folds the newest L history bits down to the index or tag
 * width, kept incrementally in folded-history registers (counted below), and
 * P folds min(L_i, 16) path-history bits to the index width.
 * Lookup: provider = longest-history table whose tag matches; alternate =
 *   next match or the bimodal counter. If the provider's counter is weak
 *   (3 or 4, i.e. newly allocated) and USE_ALT (4-bit, 0..15) >= 8, use the
 *   alternate's prediction, else the provider's.
 * Update (on every conditional branch):
 *   - USE_ALT moves toward the alternate when a weak provider and the
 *     alternate disagreed.
 *   - The provider's counter moves toward the outcome; if its u is 0 the
 *     alternate (or bimodal) is trained too. With no provider, the bimodal.
 *   - u moves toward the provider when provider and alternate disagreed.
 *   - Allocation, if TAGE's prediction was wrong: in the shortest-history
 *     table longer than the provider whose entry has u = 0, write the tag,
 *     ctr = weak toward the outcome (4 or 3), u = 0. If none has u = 0,
 *     decrement u of those entries instead.
 *   - Every 2^18 conditional branches, all u are halved (graceful aging).
 *
 * ---- Loop predictor -----------------------------------------------------
 * 16 entries (4 sets x 4 ways), each valid (1) + tag (10) + trip count (10)
 * + current iteration (10) + confidence (2) + age (4) + direction (1) = 38 bits.
 *   Lookup: set = PC mod 4, tag = (PC >> 2) mod 2^10. Prediction is "loop
 *   body direction" unless the next iteration would reach the trip count;
 *   used only at confidence 3 (three identical trip counts in a row), and
 *   only while USE_LOOP (7-bit signed) >= 0.
 *   Update: count iterations, learn and confirm the trip count at each exit;
 *   free the entry on a wrong confident prediction. Allocate (direction =
 *   the opposite of the outcome) when TAGE mispredicts a branch that has no
 *   entry, in a way with age 0; otherwise age the set's entries.
 *   USE_LOOP moves toward the loop predictor whenever it and TAGE disagree.
 *
 * ---- Statistical corrector ----------------------------------------------
 * Bias table: 2^sc_bias_log 6-bit signed counters, index from (PC, prediction
 * so far). GEHL tables: 6-bit signed counters, index = PC XOR folded global
 * history of sc_hist[j] bits.
 *   sum = sum over tables of (2c + 1). SC's prediction = sum >= 0.
 *   If it disagrees with the prediction so far, SC overrides only with
 *   enough evidence for that prediction's confidence (provider counter
 *   saturated = high, one step from saturated = medium, else low; a loop
 *   prediction counts as high): |sum| >= theta/4 for low, theta/2 for
 *   medium, theta for high.
 *   Training: when SC's prediction was wrong or |sum| < theta.
 *   theta (8-bit, starts at 20) adapts with a 6-bit signed counter TC, only
 *   when SC disagreed with the prediction so far: up if SC was wrong, down
 *   if it was right.
 * Simplifications versus Seznec's TAGE-SC-L: one allocation per
 * misprediction (not randomized), no local-history or IMLI components in
 * the SC, no speculative history management (the simulator is correct-path).
 *
 * ---- Storage ------------------------------------------------------------
 * Counted: bimodal, tagged tables, global and path history, every folded-
 * history register, USE_ALT, the 18-bit aging tick, loop table and USE_LOOP,
 * SC tables, theta and TC. See StorageBits(); tests check the totals.
 */
#ifndef HW3_TAGE_H
#define HW3_TAGE_H

#include <stdint.h>
#include <iomanip>
#include <ostream>
#include <string>
#include <vector>

#include "predictor.h"

struct TageConfig {
  unsigned bim_log   = 12;
  unsigned table_log = 9;
  std::vector<unsigned> hist_len{4, 8, 17, 37, 77, 160};
  std::vector<unsigned> tag_bits{8, 8, 9, 9, 10, 10};
  bool use_loop = true;
  bool use_sc   = true;
  unsigned sc_bias_log = 9;
  unsigned sc_gehl_log = 8;
  std::vector<unsigned> sc_hist{10, 20};   /* at most kMaxSc entries */
};

class TagePredictor : public DirectionPredictor {
 public:
  static const unsigned kMaxTables = 8, kHistBuf = 1024, kPathBits = 16;
  static const unsigned kLoopSets = 4, kLoopWays = 4, kLoopEntryBits = 38;
  static const unsigned kTickBits = 18, kMaxSc = 4;

  explicit TagePredictor(const TageConfig &c)
    : c_(c), n_((unsigned)c.hist_len.size()), bim_(1u << c.bim_log, 1),
      ghist_(kHistBuf, 0), ptr_(0), recent_(0), phist_(0), use_alt_(8), tick_(0),
      loop_(kLoopSets * kLoopWays), use_loop_(-1),
      sc_bias_(1u << c.sc_bias_log, 0), theta_(20), tc_(0)
  {
    for (unsigned i = 0; i < n_; i++) {
      tables_.push_back(std::vector<Entry>(1u << c.table_log));
      fidx_[i].Init(c.hist_len[i], c.table_log);
      ftag1_[i].Init(c.hist_len[i], c.tag_bits[i]);
      ftag2_[i].Init(c.hist_len[i], c.tag_bits[i] - 1);
    }
    for (size_t j = 0; j < c.sc_hist.size(); j++) {
      sc_gehl_.push_back(std::vector<int8_t>(1u << c.sc_gehl_log, 0));
      Folded f;
      f.Init(c.sc_hist[j], c.sc_gehl_log);
      fsc_.push_back(f);
    }
    for (unsigned i = 0; i <= kMaxTables; i++) prov_count_[i] = prov_wrong_[i] = 0;
  }

  bool Predict(uint64_t pc) const override { return Lookup(pc).final_pred; }

  uint64_t History() const override { return recent_; }

  void Update(uint64_t pc, bool taken) override
  {
    Look L = Lookup(pc);
    unsigned slot = L.provider >= 0 ? (unsigned)L.provider + 1 : 0;
    prov_count_[slot]++;
    if (L.tage_pred != taken) prov_wrong_[slot]++;
    if (L.used_alt) alt_used_++;
    if (L.loop_override) { loop_overrides_++; if (L.loop_pred == taken) loop_override_right_++; }
    if (L.sc_override) { sc_overrides_++; if (L.final_pred == taken) sc_override_right_++; }

    UpdateTage(pc, L, taken);
    if (c_.use_loop) UpdateLoop(pc, L, taken);
    if (c_.use_sc) UpdateSc(L, taken);
    PushHistory(pc, taken);
  }

  uint64_t StorageBits() const override
  {
    return BimBits() + TaggedBits() + HistoryBits() + FoldedBits() + 4 + kTickBits +
           LoopBits() + ScBits();
  }

  void PrintConfig(std::ostream &os) const override
  {
    Row(os, "Bimodal base table", BimBits(), "(" + Num(1u << c_.bim_log) + " x 2)");
    std::string lens, tags;
    for (unsigned i = 0; i < n_; i++) {
      lens += (i ? "," : "") + Num(c_.hist_len[i]);
      tags += (i ? "," : "") + Num(c_.tag_bits[i]);
    }
    Row(os, "Tagged tables", TaggedBits(),
        "(" + Num(n_) + " x " + Num(1u << c_.table_log) + " x (3 ctr + 2 u + tag); histories " +
        lens + "; tags " + tags + ")");
    Row(os, "Global + path history", HistoryBits(),
        "(" + Num(c_.hist_len[n_ - 1]) + " + " + Num(kPathBits) + ")");
    Row(os, "Folded-history registers", FoldedBits(), "");
    Row(os, "USE_ALT + aging tick", 4 + kTickBits, "(4 + 18)");
    if (c_.use_loop)
      Row(os, "Loop predictor", LoopBits(), "(16 x 38 + 7-bit USE_LOOP)");
    if (c_.use_sc)
      Row(os, "Statistical corrector", ScBits(),
          "(bias " + Num(1u << c_.sc_bias_log) + " x 6 + " + Num((unsigned)c_.sc_hist.size()) +
          " x " + Num(1u << c_.sc_gehl_log) + " x 6 + theta 8 + TC 6)");
    Row(os, "Direction predictor total", StorageBits(), "");
  }

  void PrintDiagnostics(std::ostream &os) const override
  {
    uint64_t total = 0;
    for (unsigned i = 0; i <= n_; i++) total += prov_count_[i];
    os << "  TAGE provider (conditional branches)   predictions   TAGE wrong   wrong%\n";
    for (unsigned i = 0; i <= n_; i++) {
      std::string name = i == 0 ? std::string("bimodal (no tag match)")
                                : "table " + Num(i) + " (history " + Num(c_.hist_len[i - 1]) + ")";
      os << "    " << std::left << std::setw(34) << name << std::right << std::setw(14)
         << prov_count_[i] << std::setw(13) << prov_wrong_[i] << std::setw(9) << std::fixed
         << std::setprecision(2) << (prov_count_[i] ? 100.0 * prov_wrong_[i] / prov_count_[i] : 0.0)
         << "\n";
      os.unsetf(std::ios::floatfield);
    }
    os << "    alternate used for a weak provider: " << alt_used_ << " of " << total << "\n";
    if (c_.use_loop)
      os << "  Loop predictor overrode TAGE " << loop_overrides_ << " times, right "
         << loop_override_right_ << "\n";
    if (c_.use_sc)
      os << "  Statistical corrector overrode " << sc_overrides_ << " times, right "
         << sc_override_right_ << "; final theta " << theta_ << "\n";
  }

 private:
  struct Entry { uint16_t tag = 0; uint8_t ctr = 3, u = 0; };
  struct LoopEntry {
    bool valid = false, dir = false;
    uint16_t tag = 0, nbiter = 0, curiter = 0;
    uint8_t conf = 0, age = 0;
  };

  /* Folded global history (Seznec's circular-shift register). */
  struct Folded {
    unsigned comp = 0, clen = 1, olen = 0, outpoint = 0;
    void Init(unsigned original, unsigned compressed)
    {
      olen = original; clen = compressed; outpoint = olen % clen; comp = 0;
    }
    void Update(unsigned newest, unsigned leaving)
    {
      comp = (comp << 1) ^ newest;
      comp ^= leaving << outpoint;
      comp ^= comp >> clen;
      comp &= (1u << clen) - 1;
    }
  };

  struct Look {
    unsigned idx[kMaxTables];
    uint16_t tag[kMaxTables];
    int provider = -1, alt = -1;
    bool bim_pred = false, alt_pred = false, prov_pred = false, weak = false;
    bool used_alt = false, tage_pred = false;
    int conf = 0;                      /* 0 low, 1 medium, 2 high */
    int loop_way = -1;
    bool loop_valid = false, loop_pred = false, loop_override = false;
    bool pred_inter = false;           /* after the loop predictor */
    unsigned bias_idx = 0;
    unsigned gehl_idx[kMaxSc];
    int sum = 0;
    bool sc_pred = false, sc_override = false, final_pred = false;
  };

  unsigned Bit(unsigned i) const { return ghist_[(ptr_ + i) & (kHistBuf - 1)]; }

  Look Lookup(uint64_t pc) const
  {
    Look L;
    for (unsigned i = 0; i < n_; i++) {
      uint64_t p = phist_ & bp::LowMask(c_.hist_len[i] < kPathBits ? c_.hist_len[i] : kPathBits);
      p ^= p >> c_.table_log;
      L.idx[i] = (unsigned)((pc ^ (pc >> (c_.table_log - i)) ^ fidx_[i].comp ^ p) &
                            bp::LowMask(c_.table_log));
      L.tag[i] = (uint16_t)((pc ^ ftag1_[i].comp ^ (ftag2_[i].comp << 1)) &
                            bp::LowMask(c_.tag_bits[i]));
    }
    for (int i = (int)n_ - 1; i >= 0; i--) {
      if (tables_[i][L.idx[i]].tag == L.tag[i]) {
        if (L.provider < 0) L.provider = i;
        else { L.alt = i; break; }
      }
    }
    uint8_t b = bim_[pc & bp::LowMask(c_.bim_log)];
    L.bim_pred = b >= 2;
    L.alt_pred = L.alt >= 0 ? tables_[L.alt][L.idx[L.alt]].ctr >= 4 : L.bim_pred;
    if (L.provider >= 0) {
      uint8_t ctr = tables_[L.provider][L.idx[L.provider]].ctr;
      L.prov_pred = ctr >= 4;
      L.weak = (ctr == 3 || ctr == 4);
      L.used_alt = L.weak && use_alt_ >= 8;
      L.tage_pred = L.used_alt ? L.alt_pred : L.prov_pred;
      int strength = 2 * ctr - 7;
      if (strength < 0) strength = -strength;
      L.conf = L.used_alt ? 0 : (strength == 7 ? 2 : (strength == 5 ? 1 : 0));
    } else {
      L.tage_pred = L.bim_pred;
      L.conf = (b == 0 || b == 3) ? 1 : 0;
    }

    L.pred_inter = L.tage_pred;
    if (c_.use_loop) {
      unsigned set = (unsigned)(pc & (kLoopSets - 1));
      uint16_t tag = (uint16_t)((pc >> 2) & 0x3ff);
      for (unsigned w = 0; w < kLoopWays; w++) {
        const LoopEntry &e = loop_[set * kLoopWays + w];
        if (e.valid && e.tag == tag) {
          L.loop_way = (int)(set * kLoopWays + w);
          L.loop_valid = (e.conf == 3);
          L.loop_pred = (e.curiter + 1 == e.nbiter) ? !e.dir : e.dir;
          break;
        }
      }
      if (L.loop_valid && use_loop_ >= 0) {
        L.loop_override = (L.loop_pred != L.tage_pred);
        L.pred_inter = L.loop_pred;
        L.conf = 2;
      }
    }

    L.final_pred = L.pred_inter;
    if (c_.use_sc) {
      L.bias_idx = (unsigned)((((pc ^ (pc >> c_.sc_bias_log)) << 1) | (L.pred_inter ? 1 : 0)) &
                              bp::LowMask(c_.sc_bias_log));
      L.sum = 2 * sc_bias_[L.bias_idx] + 1;
      for (size_t j = 0; j < sc_gehl_.size(); j++) {
        unsigned gi = (unsigned)((pc ^ (pc >> c_.sc_gehl_log) ^ fsc_[j].comp) &
                                 bp::LowMask(c_.sc_gehl_log));
        L.gehl_idx[j] = gi;
        L.sum += 2 * sc_gehl_[j][gi] + 1;
      }
      L.sc_pred = L.sum >= 0;
      int mag = L.sum < 0 ? -L.sum : L.sum;
      if (L.sc_pred != L.pred_inter) {
        bool flip = (L.conf == 0 && mag >= theta_ / 4) || (L.conf == 1 && mag >= theta_ / 2) ||
                    (L.conf == 2 && mag >= theta_);
        if (flip) {
          L.final_pred = L.sc_pred;
          L.sc_override = true;
        }
      }
    }
    return L;
  }

  void UpdateTage(uint64_t pc, const Look &L, bool taken)
  {
    if (L.provider >= 0 && L.weak && L.prov_pred != L.alt_pred) {
      if (L.alt_pred == taken) { if (use_alt_ < 15) use_alt_++; }
      else if (use_alt_ > 0) use_alt_--;
    }

    if (L.tage_pred != taken && L.provider < (int)n_ - 1) {
      bool done = false;
      for (unsigned j = (unsigned)(L.provider + 1); j < n_ && !done; j++) {
        Entry &e = tables_[j][L.idx[j]];
        if (e.u == 0) {
          e.tag = L.tag[j];
          e.ctr = taken ? 4 : 3;
          done = true;
        }
      }
      if (!done)
        for (unsigned j = (unsigned)(L.provider + 1); j < n_; j++) {
          Entry &e = tables_[j][L.idx[j]];
          if (e.u > 0) e.u--;
        }
    }

    unsigned bi = (unsigned)(pc & bp::LowMask(c_.bim_log));
    if (L.provider >= 0) {
      Entry &p = tables_[L.provider][L.idx[L.provider]];
      if (p.u == 0) {
        if (L.alt >= 0) {
          Entry &a = tables_[L.alt][L.idx[L.alt]];
          a.ctr = bp::SatUpdate(a.ctr, taken, 3);
        } else {
          bim_[bi] = bp::SatUpdate(bim_[bi], taken, 2);
        }
      }
      p.ctr = bp::SatUpdate(p.ctr, taken, 3);
      if (L.prov_pred != L.alt_pred) {
        if (L.prov_pred == taken) { if (p.u < 3) p.u++; }
        else if (p.u > 0) p.u--;
      }
    } else {
      bim_[bi] = bp::SatUpdate(bim_[bi], taken, 2);
    }

    tick_ = (tick_ + 1) & bp::LowMask(kTickBits);
    if (tick_ == 0)
      for (unsigned i = 0; i < n_; i++)
        for (size_t k = 0; k < tables_[i].size(); k++) tables_[i][k].u >>= 1;
  }

  void UpdateLoop(uint64_t pc, const Look &L, bool taken)
  {
    if (L.loop_valid && L.loop_pred != L.tage_pred) {
      if (L.loop_pred == taken) { if (use_loop_ < 63) use_loop_++; }
      else if (use_loop_ > -64) use_loop_--;
    }
    if (L.loop_way >= 0) {
      LoopEntry &e = loop_[L.loop_way];
      if (L.loop_valid) {
        if (taken != L.loop_pred) {
          e.nbiter = 0; e.age = 0; e.conf = 0; e.curiter = 0;
          return;
        }
        if (L.loop_pred != L.tage_pred && e.age < 15) e.age++;
      }
      e.curiter = (uint16_t)((e.curiter + 1) & 0x3ff);
      if (e.curiter > e.nbiter) { e.conf = 0; e.nbiter = 0; }
      if (taken != e.dir) {
        if (e.curiter == e.nbiter) {
          if (e.conf < 3) e.conf++;
          if (e.nbiter < 3) { e.dir = taken; e.nbiter = 0; e.age = 0; e.conf = 0; }
        } else if (e.nbiter == 0) {
          e.conf = 0;
          e.nbiter = e.curiter;
        } else {
          e.nbiter = 0;
          e.conf = 0;
        }
        e.curiter = 0;
      }
    } else if (L.tage_pred != taken) {
      unsigned set = (unsigned)(pc & (kLoopSets - 1));
      bool done = false;
      for (unsigned w = 0; w < kLoopWays && !done; w++) {
        LoopEntry &e = loop_[set * kLoopWays + w];
        if (!e.valid || e.age == 0) {
          e.valid = true; e.tag = (uint16_t)((pc >> 2) & 0x3ff); e.dir = !taken;
          e.nbiter = 0; e.curiter = 0; e.conf = 0; e.age = 7;
          done = true;
        }
      }
      if (!done)
        for (unsigned w = 0; w < kLoopWays; w++) {
          LoopEntry &e = loop_[set * kLoopWays + w];
          if (e.age > 0) e.age--;
        }
    }
  }

  void UpdateSc(const Look &L, bool taken)
  {
    int mag = L.sum < 0 ? -L.sum : L.sum;
    if (L.sc_pred != taken || mag < theta_) {
      SatSigned(sc_bias_[L.bias_idx], taken);
      for (size_t j = 0; j < sc_gehl_.size(); j++) SatSigned(sc_gehl_[j][L.gehl_idx[j]], taken);
    }
    /* Adapt theta only when SC disagreed with the prediction so far, the
       only case where it can change the outcome: SC wrong -> demand more
       evidence; SC right -> demand less. */
    if (L.sc_pred != L.pred_inter) {
      if (L.sc_pred != taken) {
        if (++tc_ >= 31) { if (theta_ < 255) theta_++; tc_ = 0; }
      } else if (--tc_ <= -32) {
        if (theta_ > 4) theta_--;
        tc_ = 0;
      }
    }
  }

  static void SatSigned(int8_t &c, bool up)
  {
    if (up) { if (c < 31) c++; }
    else if (c > -32) c--;
  }

  void PushHistory(uint64_t pc, bool taken)
  {
    ptr_ = (ptr_ + kHistBuf - 1) & (kHistBuf - 1);
    ghist_[ptr_] = taken ? 1 : 0;
    recent_ = (recent_ << 1) | (taken ? 1 : 0);
    phist_ = ((phist_ << 1) ^ ((pc ^ (pc >> 1)) & 1)) & bp::LowMask(kPathBits);
    for (unsigned i = 0; i < n_; i++) {
      unsigned leaving = Bit(c_.hist_len[i]);
      fidx_[i].Update(Bit(0), leaving);
      ftag1_[i].Update(Bit(0), leaving);
      ftag2_[i].Update(Bit(0), leaving);
    }
    for (size_t j = 0; j < fsc_.size(); j++) fsc_[j].Update(Bit(0), Bit(c_.sc_hist[j]));
  }

  uint64_t BimBits() const { return (uint64_t)2 << c_.bim_log; }
  uint64_t TaggedBits() const
  {
    uint64_t b = 0;
    for (unsigned i = 0; i < n_; i++) b += (uint64_t)(1u << c_.table_log) * (5 + c_.tag_bits[i]);
    return b;
  }
  uint64_t HistoryBits() const { return c_.hist_len[n_ - 1] + kPathBits; }
  uint64_t FoldedBits() const
  {
    uint64_t b = 0;
    for (unsigned i = 0; i < n_; i++) b += c_.table_log + c_.tag_bits[i] + (c_.tag_bits[i] - 1);
    if (c_.use_sc) b += c_.sc_hist.size() * c_.sc_gehl_log;
    return b;
  }
  uint64_t LoopBits() const
  {
    return c_.use_loop ? kLoopSets * kLoopWays * kLoopEntryBits + 7 : 0;
  }
  uint64_t ScBits() const
  {
    if (!c_.use_sc) return 0;
    return (uint64_t)6 * ((1u << c_.sc_bias_log) + c_.sc_hist.size() * (1u << c_.sc_gehl_log)) +
           8 + 6;
  }

  static std::string Num(uint64_t v)
  {
    std::string s;
    do { s.insert(s.begin(), (char)('0' + v % 10)); v /= 10; } while (v);
    return s;
  }
  static void Row(std::ostream &os, const char *name, uint64_t bits, const std::string &detail)
  {
    os << "  " << std::left << std::setw(34) << name << std::right << std::setw(10) << bits
       << "   " << detail << "\n";
  }

  TageConfig c_;
  unsigned n_;
  std::vector<std::vector<Entry>> tables_;
  std::vector<uint8_t> bim_;
  std::vector<uint8_t> ghist_;
  unsigned ptr_;
  uint64_t recent_, phist_;
  Folded fidx_[kMaxTables], ftag1_[kMaxTables], ftag2_[kMaxTables];
  unsigned use_alt_, tick_;
  std::vector<LoopEntry> loop_;
  int use_loop_;
  std::vector<int8_t> sc_bias_;
  std::vector<std::vector<int8_t>> sc_gehl_;
  std::vector<Folded> fsc_;
  int theta_, tc_;
  uint64_t prov_count_[kMaxTables + 1], prov_wrong_[kMaxTables + 1];
  uint64_t alt_used_ = 0, loop_overrides_ = 0, loop_override_right_ = 0;
  uint64_t sc_overrides_ = 0, sc_override_right_ = 0;
};

#endif
