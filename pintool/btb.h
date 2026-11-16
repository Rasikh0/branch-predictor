/*
 * btb.h -- branch target buffer.
 *
 * Baseline (the defaults): 256 entries, direct-mapped, each entry
 *   valid (1) + tag (16) + target (32) + type (3) = 52 bits
 *   256 x 52 = 13,312 bits
 *
 * Choices (see report/design_notes.md):
 *   - Index = low PC bits, tag = the next tag_bits bits. x86 instructions can
 *     start at any byte, so the PC is not shifted first.
 *   - Only the low target_bits bits of the target are stored; the upper bits
 *     are restored from the branch's own PC (bp::Restore).
 *   - Entries are allocated only for taken branches. On a hit, target and type
 *     are refreshed, so indirect branches keep their most recent target.
 *   - With more than one way, replacement is LRU, using a log2(ways)-bit age
 *     per entry (counted in the storage). Direct-mapped needs no such state.
 */
#ifndef HW3_BTB_H
#define HW3_BTB_H

#include <stdint.h>
#include <iomanip>
#include <ostream>
#include <vector>

#include "predictor.h"

struct BtbResult {
  bool     hit    = false;
  bool     alias  = false;  /* diagnostic: tag matched but entry belongs to another PC */
  uint64_t target = 0;      /* restored full target */
  int      type   = -1;
};

class Btb {
 public:
  static const unsigned kTypeBits = 3;

  Btb(unsigned entries = 256, unsigned ways = 1, unsigned tag_bits = 16,
      unsigned target_bits = 32)
    : entries_(entries), ways_(ways), sets_(entries / ways),
      index_bits_(bp::Log2(entries / ways)), tag_bits_(tag_bits),
      target_bits_(target_bits), lru_bits_(ways > 1 ? bp::Log2(ways) : 0),
      table_(entries) {}

  BtbResult Lookup(uint64_t pc) const
  {
    BtbResult r;
    const Entry *e = Find(pc);
    if (e == 0) return r;
    r.hit    = true;
    r.alias  = (e->shadow_pc != pc);
    r.target = bp::Restore(pc, e->target, target_bits_);
    r.type   = e->type;
    return r;
  }

  /* Record a taken branch: refresh its entry, or allocate one. */
  void Update(uint64_t pc, uint64_t target, int type)
  {
    Entry *e = Find(pc);
    bool present = (e != 0);
    if (!present) e = Victim(pc);
    Touch(pc, e, present);
    e->valid     = true;
    e->tag       = Tag(pc);
    e->target    = bp::Truncate(target, target_bits_);
    e->type      = type;
    e->shadow_pc = pc;
  }

  uint64_t BitsPerEntry() const { return 1 + tag_bits_ + target_bits_ + kTypeBits + lru_bits_; }
  uint64_t StorageBits() const  { return (uint64_t)entries_ * BitsPerEntry(); }

  void PrintConfig(std::ostream &os) const
  {
    os << "  " << std::left << std::setw(34) << "BTB" << std::right << std::setw(10)
       << StorageBits() << "   (" << entries_ << " x (1 valid + " << tag_bits_ << " tag + "
       << target_bits_ << " target + " << kTypeBits << " type";
    if (lru_bits_) os << " + " << lru_bits_ << " LRU";
    os << "), " << sets_ << " sets x " << ways_ << " ways\n";
  }

 private:
  struct Entry {
    bool     valid     = false;
    uint64_t tag       = 0;
    uint64_t target    = 0;
    int      type      = 0;
    unsigned age       = 0;  /* LRU age: 0 = most recent */
    uint64_t shadow_pc = 0;  /* full PC, for the aliasing diagnostic only (not counted) */
  };

  unsigned Set(uint64_t pc) const { return (unsigned)(pc & (sets_ - 1)); }
  uint64_t Tag(uint64_t pc) const { return (pc >> index_bits_) & bp::LowMask(tag_bits_); }

  const Entry *Find(uint64_t pc) const
  {
    unsigned base = Set(pc) * ways_;
    uint64_t tag = Tag(pc);
    for (unsigned w = 0; w < ways_; w++) {
      const Entry &e = table_[base + w];
      if (e.valid && e.tag == tag) return &e;
    }
    return 0;
  }
  Entry *Find(uint64_t pc) { return const_cast<Entry *>(static_cast<const Btb *>(this)->Find(pc)); }

  /* Invalid way if any, otherwise the least recently used. */
  Entry *Victim(uint64_t pc)
  {
    unsigned base = Set(pc) * ways_;
    Entry *victim = &table_[base];
    for (unsigned w = 0; w < ways_; w++) {
      Entry *e = &table_[base + w];
      if (!e->valid) return e;
      if (e->age > victim->age) victim = e;
    }
    return victim;
  }

  /* Make e the most recent in its set. Entries that were more recent than e
     age by one; a newly filled entry was older than all of them. */
  void Touch(uint64_t pc, Entry *e, bool present)
  {
    if (ways_ == 1) return;
    unsigned base = Set(pc) * ways_;
    unsigned old = present ? e->age : ways_;
    for (unsigned w = 0; w < ways_; w++) {
      Entry &o = table_[base + w];
      if (&o != e && o.valid && o.age < old) o.age++;
    }
    e->age = 0;
  }

  unsigned entries_, ways_, sets_, index_bits_, tag_bits_, target_bits_, lru_bits_;
  std::vector<Entry> table_;
};

#endif
