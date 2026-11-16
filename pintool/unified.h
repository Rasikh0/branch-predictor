/*
 * unified.h -- Section 6, option C: a unified context-indexed target predictor.
 *
 * One set-associative table predicts the targets of all branches (direct and
 * indirect), replacing the separate BTB and indirect target predictor. Returns
 * still take their target from the RAS; the table only identifies them.
 *
 * Context H (as in itp4_gh): PHR of recent indirect targets, optionally XOR
 * recent conditional outcomes from the GHR, 14 bits.
 *
 * Two modes:
 *   HIST_ALL  (uni_hist with GHR bits, uni_phr without): every lookup and
 *             update uses the hashed key (PC, H). One probe per branch, but a
 *             branch occupies one entry per context it is seen in.
 *   TWO_PROBE (uni_2probe): probe 1 uses PC alone; if that entry says the
 *             branch is indirect, probe 2 uses (PC, H) in the same table.
 *             Direct branches occupy one entry; indirect branches need two
 *             dependent lookups. A 1-bit kind field tells PC entries from
 *             context entries.
 *
 * Keys (sets = 128, ib = 7):
 *   PC key:      set = PC mod 128;                       tag = (PC >> 7) mod 2^16
 *   context key: set = (PC XOR H XOR (H >> 7)) mod 128;  tag = ((PC >> 7) XOR (H >> 7) XOR (H << 1)) mod 2^16
 *
 * Entry: valid (1) + [kind (1), TWO_PROBE only] + tag (16) + target (32)
 *        + type (3) + LRU age (3, for 5 ways).
 * Defaults: 128 sets x 5 ways = 640 entries.
 *   HIST_ALL:  640 x 55 + 14 = 35,214 bits
 *   TWO_PROBE: 640 x 56 + 14 = 35,854 bits (= BTB 4-way 13,824 + itp4 22,030)
 */
#ifndef HW3_UNIFIED_H
#define HW3_UNIFIED_H

#include <stdint.h>
#include <iomanip>
#include <ostream>
#include <set>
#include <vector>

#include "branch_event.h"
#include "predictor.h"

class UnifiedTargetTable {
 public:
  enum Mode { HIST_ALL = 1, TWO_PROBE = 2 };

  struct Result {
    bool     hit    = false;
    bool     alias  = false;  /* diagnostic: hit an entry of another branch/context */
    uint64_t target = 0;
    int      type   = -1;
  };

  UnifiedTargetTable(int mode = HIST_ALL, unsigned sets = 128, unsigned ways = 5,
                     unsigned tag_bits = 16, unsigned target_bits = 32,
                     unsigned hist_bits = 14, unsigned ghr_bits = 8)
    : mode_(mode), sets_(sets), ways_(ways), index_bits_(bp::Log2(sets)),
      tag_bits_(tag_bits), target_bits_(target_bits), hist_bits_(hist_bits),
      ghr_bits_(ghr_bits), lru_bits_(ways > 1 ? bp::Log2(ways) : 0),
      kind_bits_(mode == TWO_PROBE ? 1 : 0), table_(sets * ways), phr_(0),
      lookups_(0), second_probes_(0) {}

  Result Lookup(uint64_t pc, uint64_t ghr)
  {
    lookups_++;
    Result r;
    uint64_t h = Context(ghr);
    if (mode_ == HIST_ALL) {
      const Entry *e = Find(CtxSet(pc, h), CtxTag(pc, h), 1);
      if (e) Fill(r, pc, *e, pc, h);
      return r;
    }
    const Entry *e1 = Find(PcSet(pc), PcTag(pc), 0);
    if (!e1) return r;
    Fill(r, pc, *e1, pc, 0);
    if (IsIndirect(e1->type)) {
      second_probes_++;
      const Entry *e2 = Find(CtxSet(pc, h), CtxTag(pc, h), 1);
      if (e2) {
        r.target = bp::Restore(pc, e2->target, target_bits_);
        r.alias  = r.alias || e2->shadow_pc != pc || e2->shadow_ctx != h;
      }
    }
    return r;
  }

  /* Train with the actual outcome. Allocation only for taken branches. */
  void Update(uint64_t pc, uint64_t ghr, uint64_t target, int type, bool taken)
  {
    uint64_t h = Context(ghr);
    if (taken) {
      if (mode_ == HIST_ALL) {
        Write(CtxSet(pc, h), CtxTag(pc, h), 1, pc, h, target, type);
      } else {
        Write(PcSet(pc), PcTag(pc), 0, pc, 0, target, type);
        if (IsIndirect(type)) Write(CtxSet(pc, h), CtxTag(pc, h), 1, pc, h, target, type);
      }
    }
    if (IsIndirect(type)) phr_ = ((phr_ << 2) ^ Mix(target)) & bp::LowMask(hist_bits_);
  }

  uint64_t BitsPerEntry() const
  {
    return 1 + kind_bits_ + tag_bits_ + target_bits_ + 3 + lru_bits_;
  }
  uint64_t StorageBits() const { return (uint64_t)sets_ * ways_ * BitsPerEntry() + hist_bits_; }

  void PrintConfig(std::ostream &os) const
  {
    os << "  " << std::left << std::setw(34) << "Unified target table" << std::right
       << std::setw(10) << StorageBits() << "   (" << sets_ * ways_ << " x (1 valid + ";
    if (kind_bits_) os << "1 kind + ";
    os << tag_bits_ << " tag + " << target_bits_ << " target + 3 type + " << lru_bits_
       << " LRU) + " << hist_bits_ << "-bit path history, " << sets_ << " sets x " << ways_
       << " ways, " << (mode_ == HIST_ALL ? "every lookup uses (PC, history)"
                                          : "PC probe, then (PC, history) probe if indirect");
    if (ghr_bits_) os << "; context uses " << ghr_bits_ << " GHR bits";
    os << ")\n";
  }

  /* Table contents at the end of the run: how many entries each kind of
     branch occupies, and how many distinct branches they hold. */
  void PrintDiagnostics(std::ostream &os) const
  {
    uint64_t n[3] = {0, 0, 0};
    std::set<uint64_t> pcs[3];
    for (size_t i = 0; i < table_.size(); i++) {
      const Entry &e = table_[i];
      if (!e.valid) continue;
      int c = IsIndirect(e.type) ? 1 : (e.type == BR_RET ? 2 : 0);
      n[c]++;
      pcs[c].insert(e.shadow_pc);
    }
    const char *names[3] = {"direct (cond, jmp, call)", "indirect (jmp, call)", "return"};
    os << "  Unified table at end of run (" << table_.size() << " entries)\n";
    os << "    " << std::left << std::setw(26) << "branch kind" << std::right << std::setw(10)
       << "entries" << std::setw(12) << "branches" << std::setw(18) << "entries/branch" << "\n";
    for (int c = 0; c < 3; c++) {
      os << "    " << std::left << std::setw(26) << names[c] << std::right << std::setw(10)
         << n[c] << std::setw(12) << pcs[c].size() << std::setw(18) << std::fixed
         << std::setprecision(2) << (pcs[c].empty() ? 0.0 : (double)n[c] / pcs[c].size())
         << "\n";
      os.unsetf(std::ios::floatfield);
    }
    os << "  Unified table lookups " << lookups_ << ", second (dependent) probes "
       << second_probes_ << "\n";
  }

 private:
  struct Entry {
    bool     valid      = false;
    int      kind       = 0;    /* 0 = PC entry, 1 = context entry */
    uint64_t tag        = 0;
    uint64_t target     = 0;
    int      type       = 0;
    unsigned age        = 0;
    uint64_t shadow_pc  = 0;    /* diagnostics only, not counted in storage */
    uint64_t shadow_ctx = 0;
  };

  static bool IsIndirect(int t) { return t == BR_JMP_INDIRECT || t == BR_CALL_INDIRECT; }
  static uint64_t Mix(uint64_t t) { return t ^ (t >> 4) ^ (t >> 10); }

  uint64_t Context(uint64_t ghr) const
  {
    return (phr_ ^ ((ghr & bp::LowMask(ghr_bits_)) << 4)) & bp::LowMask(hist_bits_);
  }
  unsigned PcSet(uint64_t pc) const { return (unsigned)(pc & (sets_ - 1)); }
  uint64_t PcTag(uint64_t pc) const { return (pc >> index_bits_) & bp::LowMask(tag_bits_); }
  unsigned CtxSet(uint64_t pc, uint64_t h) const
  {
    uint64_t x = pc ^ h;
    for (unsigned sh = index_bits_; sh < hist_bits_; sh += index_bits_) x ^= h >> sh;
    return (unsigned)(x & (sets_ - 1));
  }
  uint64_t CtxTag(uint64_t pc, uint64_t h) const
  {
    uint64_t t = (pc >> index_bits_) ^ (h >> index_bits_) ^ (h << 1);
    if (index_bits_ + tag_bits_ < hist_bits_) t ^= h >> (index_bits_ + tag_bits_);
    return t & bp::LowMask(tag_bits_);
  }

  /* In HIST_ALL every entry is a context entry, so kinds always match. */
  const Entry *Find(unsigned set, uint64_t tag, int kind) const
  {
    for (unsigned w = 0; w < ways_; w++) {
      const Entry &e = table_[set * ways_ + w];
      if (e.valid && e.tag == tag && (mode_ == HIST_ALL || e.kind == kind)) return &e;
    }
    return 0;
  }

  void Fill(Result &r, uint64_t pc, const Entry &e, uint64_t want_pc, uint64_t want_ctx) const
  {
    r.hit    = true;
    r.target = bp::Restore(pc, e.target, target_bits_);
    r.type   = e.type;
    r.alias  = e.shadow_pc != want_pc ||
               (mode_ == HIST_ALL && e.shadow_ctx != want_ctx);
  }

  void Write(unsigned set, uint64_t tag, int kind, uint64_t pc, uint64_t ctx, uint64_t target,
             int type)
  {
    Entry *e = const_cast<Entry *>(Find(set, tag, kind));
    bool present = (e != 0);
    if (!present) {
      e = &table_[set * ways_];
      for (unsigned w = 0; w < ways_; w++) {
        Entry *c = &table_[set * ways_ + w];
        if (!c->valid) { e = c; break; }
        if (c->age > e->age) e = c;
      }
    }
    unsigned old = present ? e->age : ways_;
    for (unsigned w = 0; w < ways_; w++) {
      Entry &o = table_[set * ways_ + w];
      if (&o != e && o.valid && o.age < old) o.age++;
    }
    e->age        = 0;
    e->valid      = true;
    e->kind       = kind;
    e->tag        = tag;
    e->target     = bp::Truncate(target, target_bits_);
    e->type       = type;
    e->shadow_pc  = pc;
    e->shadow_ctx = ctx;
  }

  int mode_;
  unsigned sets_, ways_, index_bits_, tag_bits_, target_bits_, hist_bits_, ghr_bits_;
  unsigned lru_bits_, kind_bits_;
  std::vector<Entry> table_;
  uint64_t phr_;
  uint64_t lookups_, second_probes_;
};

#endif
