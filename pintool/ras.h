/*
 * ras.h -- return address stack.
 *
 * Baseline (the defaults): 16 entries x 32 bits + a 5-bit count = 517 bits.
 *
 * Behavior (see report/design_notes.md):
 *   - The count holds the number of valid entries, 0..16 (5 bits).
 *   - Push on every call, pop on every return, following the actual
 *     (correct-path) program, whether or not the prediction was right.
 *   - Overflow: pushing onto a full stack discards the oldest entry. In
 *     hardware this is a shift-register stack; the code uses an equivalent
 *     circular buffer.
 *   - Underflow: an empty stack gives no prediction (the front end then
 *     falls back to the BTB), and the pop is ignored.
 *   - Entries keep the low entry_bits bits of the return address; the upper
 *     bits are restored from the return instruction's PC.
 */
#ifndef HW3_RAS_H
#define HW3_RAS_H

#include <stdint.h>
#include <iomanip>
#include <ostream>
#include <vector>

#include "predictor.h"

class Ras {
 public:
  Ras(unsigned entries = 16, unsigned entry_bits = 32)
    : entries_(entries), entry_bits_(entry_bits), count_bits_(bp::BitsFor(entries)),
      stack_(entries, 0), top_(entries - 1), count_(0),
      pushes_(0), pops_(0), overflows_(0), underflows_(0) {}

  /* Predicted return address for a return at pc; false if the stack is empty. */
  bool Top(uint64_t pc, uint64_t *addr) const
  {
    if (count_ == 0) return false;
    *addr = bp::Restore(pc, stack_[top_], entry_bits_);
    return true;
  }

  void Push(uint64_t return_addr)
  {
    pushes_++;
    top_ = (top_ + 1) % entries_;
    stack_[top_] = bp::Truncate(return_addr, entry_bits_);
    if (count_ < entries_) count_++;
    else overflows_++;           /* the oldest entry was just overwritten */
  }

  void Pop()
  {
    pops_++;
    if (count_ == 0) {
      underflows_++;
      return;
    }
    top_ = (top_ + entries_ - 1) % entries_;
    count_--;
  }

  unsigned Count() const { return count_; }

  uint64_t StorageBits() const { return (uint64_t)entries_ * entry_bits_ + count_bits_; }

  void PrintConfig(std::ostream &os) const
  {
    os << "  " << std::left << std::setw(34) << "RAS" << std::right << std::setw(10)
       << StorageBits() << "   (" << entries_ << " x " << entry_bits_ << " + "
       << count_bits_ << "-bit count)\n";
  }

  void PrintDiagnostics(std::ostream &os) const
  {
    os << "  RAS pushes " << pushes_ << ", pops " << pops_ << ", overflows " << overflows_
       << ", underflows " << underflows_ << "\n";
  }

 private:
  unsigned entries_, entry_bits_, count_bits_;
  std::vector<uint64_t> stack_;
  unsigned top_, count_;
  uint64_t pushes_, pops_, overflows_, underflows_;
};

#endif
