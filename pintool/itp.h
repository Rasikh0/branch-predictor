/*
 * itp.h -- indirect target predictor (Section 5, improvement 2).
 *
 * Hypothesis: the baseline BTB predicts an indirect jump or call's most recent
 * target, which is wrong whenever consecutive executions go to different
 * places (an interpreter's dispatch goes to a different handler almost every
 * time). The next target is often determined by the targets just before it
 * (in an interpreter, by the last few bytecode instructions), so a table
 * indexed by the PC together with a history of recent indirect targets can
 * learn it.
 *
 * Structure (defaults):
 *   Path history register (PHR): 16 bits. On every indirect jump or call,
 *     PHR = ((PHR << 2) XOR mix(target)) mod 2^16, so it reflects roughly the
 *     last 8 indirect targets.
 *   Table: 512 entries, direct-mapped, each valid (1) + tag (8) + target (32).
 *     index = (PC XOR PHR) mod 512
 *     tag   = ((PC >> 9) XOR (PHR >> 9) XOR (PHR << 1)) mod 2^8
 *   Storage: 512 x 41 + 16 = 21,008 bits.
 *
 * Refinement 1 (itp4): measured, 'itp' still mispredicted about a quarter of
 * interp's indirect branches. An offline model of interp's exact dispatch
 * stream (scripts/model_interp_itp.py) reproduced that count and showed the
 * cause: the index used only the low 9 bits of PC XOR PHR, so many contexts
 * collided in the direct-mapped table. The refinement, same 512 entries:
 *   128 sets x 4 ways, LRU with a 2-bit age per entry
 *   14-bit context H (about the last 7 targets; a sweep in the model found
 *   12 bits too short and 16 bits split contexts needlessly)
 *   index = (PC XOR H XOR (H >> 7)) mod 128
 *   tag   = ((PC >> 7) XOR (H >> 7) XOR (H << 1)) mod 2^8
 *   so every bit of H reaches both the index and the tag.
 *   Storage: 512 x (1 + 8 + 32 + 2) + 14 = 22,030 bits.
 *
 * Refinement 2 (itp4_gh): the context also includes the 8 most recent
 * conditional outcomes from the direction predictor's global history
 * register (GHR), which already exists, so no storage is added:
 *   H = (PHR XOR (low 8 bits of GHR << 4)) mod 2^14
 * Hypothesis: when an interpreter's bytecode branch is a real conditional
 * branch, its outcome decides the next handler; target history can't see it.
 *
 * Use in the front end (frontend.h):
 *   Predict: if the BTB says the instruction is an indirect jump or call and
 *     this table hits, use its target; otherwise use the BTB's target.
 *   Update:  on every indirect jump or call, write the actual target into the
 *     entry for (PC, PHR), then shift the actual target into the PHR.
 * Targets are stored as low 32 bits and restored from the PC, like the BTB.
 */
#ifndef HW3_ITP_H
#define HW3_ITP_H

#include <stdint.h>
#include <iomanip>
#include <ostream>
#include <vector>

#include "predictor.h"

class IndirectPredictor {
 public:
  /* ways > 1: set-associative with LRU. fold: XOR the history's upper bits
     into the index. ghr_bits: how many recent conditional outcomes (from the
     GHR passed to Lookup/Update) join the context; 0 = target history only. */
  IndirectPredictor(unsigned entries = 512, unsigned tag_bits = 8, unsigned target_bits = 32,
                    unsigned hist_bits = 16, unsigned ghr_bits = 0, unsigned ways = 1,
                    bool fold = false)
    : entries_(entries), ways_(ways), sets_(entries / ways),
      index_bits_(bp::Log2(entries / ways)), tag_bits_(tag_bits), target_bits_(target_bits),
      hist_bits_(hist_bits), ghr_bits_(ghr_bits), lru_bits_(ways > 1 ? bp::Log2(ways) : 0),
      fold_(fold), table_(entries), phr_(0), lookups_(0), hits_(0) {}

  /* Target for the indirect branch at pc in the current context. */
  bool Lookup(uint64_t pc, uint64_t ghr, uint64_t *target)
  {
    lookups_++;
    uint64_t h = Context(ghr);
    const Entry *e = Find(pc, h);
    if (e == 0) return false;
    hits_++;
    *target = bp::Restore(pc, e->target, target_bits_);
    return true;
  }

  /* Learn the actual target in the current context, then extend the history. */
  void Update(uint64_t pc, uint64_t ghr, uint64_t target)
  {
    uint64_t h = Context(ghr);
    Entry *e = const_cast<Entry *>(Find(pc, h));
    bool present = (e != 0);
    if (!present) e = Victim(pc, h);
    Touch(pc, h, e, present);
    e->valid  = true;
    e->tag    = Tag(pc, h);
    e->target = bp::Truncate(target, target_bits_);
    phr_ = ((phr_ << 2) ^ Mix(target)) & bp::LowMask(hist_bits_);
  }

  uint64_t StorageBits() const
  {
    return (uint64_t)entries_ * (1 + tag_bits_ + target_bits_ + lru_bits_) + hist_bits_;
  }

  void PrintConfig(std::ostream &os) const
  {
    os << "  " << std::left << std::setw(34) << "Indirect target predictor" << std::right
       << std::setw(10) << StorageBits() << "   (" << entries_ << " x (1 valid + "
       << tag_bits_ << " tag + " << target_bits_ << " target";
    if (lru_bits_) os << " + " << lru_bits_ << " LRU";
    os << ") + " << hist_bits_ << "-bit path history, " << sets_ << " sets x " << ways_
       << " ways";
    if (fold_) os << ", folded index";
    if (ghr_bits_) os << "; also uses " << ghr_bits_ << " GHR bits";
    os << ")\n";
  }

  void PrintDiagnostics(std::ostream &os) const
  {
    os << "  Indirect target predictor lookups " << lookups_ << ", hits " << hits_ << "\n";
  }

 private:
  struct Entry {
    bool     valid  = false;
    uint64_t tag    = 0;
    uint64_t target = 0;
    unsigned age    = 0;   /* LRU age, 0 = most recent (ways > 1) */
  };

  /* Spread a target's bits so aligned addresses (low bits zero) still vary. */
  static uint64_t Mix(uint64_t t) { return t ^ (t >> 4) ^ (t >> 10); }

  uint64_t Context(uint64_t ghr) const
  {
    return (phr_ ^ ((ghr & bp::LowMask(ghr_bits_)) << 4)) & bp::LowMask(hist_bits_);
  }
  unsigned Index(uint64_t pc, uint64_t h) const
  {
    uint64_t x = pc ^ h;
    if (fold_)
      for (unsigned sh = index_bits_; sh < hist_bits_; sh += index_bits_) x ^= h >> sh;
    return (unsigned)(x & (sets_ - 1));
  }

  const Entry *Find(uint64_t pc, uint64_t h) const
  {
    unsigned base = Index(pc, h) * ways_;
    uint64_t tag = Tag(pc, h);
    for (unsigned w = 0; w < ways_; w++) {
      const Entry &e = table_[base + w];
      if (e.valid && e.tag == tag) return &e;
    }
    return 0;
  }

  /* Invalid way if any, otherwise the least recently used. */
  Entry *Victim(uint64_t pc, uint64_t h)
  {
    unsigned base = Index(pc, h) * ways_;
    Entry *victim = &table_[base];
    for (unsigned w = 0; w < ways_; w++) {
      Entry *e = &table_[base + w];
      if (!e->valid) return e;
      if (e->age > victim->age) victim = e;
    }
    return victim;
  }

  /* Make e the most recent in its set (same scheme as the BTB). */
  void Touch(uint64_t pc, uint64_t h, Entry *e, bool present)
  {
    if (ways_ == 1) return;
    unsigned base = Index(pc, h) * ways_;
    unsigned old = present ? e->age : ways_;
    for (unsigned w = 0; w < ways_; w++) {
      Entry &o = table_[base + w];
      if (&o != e && o.valid && o.age < old) o.age++;
    }
    e->age = 0;
  }
  uint64_t Tag(uint64_t pc, uint64_t h) const
  {
    uint64_t t = (pc >> index_bits_) ^ (h >> index_bits_) ^ (h << 1);
    if (fold_) t ^= h >> (index_bits_ + tag_bits_);
    return t & bp::LowMask(tag_bits_);
  }

  unsigned entries_, ways_, sets_, index_bits_, tag_bits_, target_bits_, hist_bits_, ghr_bits_;
  unsigned lru_bits_;
  bool fold_;
  std::vector<Entry> table_;
  uint64_t phr_;
  uint64_t lookups_, hits_;
};

#endif
