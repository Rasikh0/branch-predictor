/*
 * predictor.h -- the interface between the Pin tool and a front-end predictor.
 *
 * Pin-independent: the predictors can be compiled and unit-tested without Pin.
 *
 * For every control-flow event the tool calls, in this order:
 *   1. Predict(pc, seq_next)  -- using only what fetch knows: the address
 *   2. (the tool records whether the prediction was right)
 *   3. Update(event, prediction) -- train with the actual outcome
 * There is no delay between the two calls (the spec's correct-path model).
 */
#ifndef HW3_PREDICTOR_H
#define HW3_PREDICTOR_H

#include <stdint.h>
#include <ostream>

#include "branch_event.h"

/* What the front end predicts for one instruction, before decoding it. */
struct Prediction {
  uint64_t next_pc   = 0;      /* predicted next fetch address */
  bool     dir_taken = false;  /* direction predictor's guess (used for conditional branches) */
  bool     btb_hit   = false;
  int      btb_type  = -1;     /* branch type stored in the BTB entry, if it hit */
  bool     from_ras  = false;  /* a return predicted from the RAS (not the BTB fallback) */
  bool     btb_alias = false;  /* diagnostic: the hit entry belongs to a different branch
                                  (partial-tag aliasing); not visible to real hardware */
};

class FrontEndPredictor {
 public:
  virtual ~FrontEndPredictor() {}

  virtual const char *Name() const = 0;

  /* seq_next = pc + size: where fetch goes if nothing redirects it. */
  virtual Prediction Predict(uint64_t pc, uint64_t seq_next) = 0;

  /* Train every structure with the actual outcome. */
  virtual void Update(const BranchEvent &ev, const Prediction &p) = 0;

  /* Total persistent predictor state, in bits. */
  virtual uint64_t StorageBits() const = 0;

  /* Structures and their bit counts. */
  virtual void PrintConfig(std::ostream &os) const = 0;

  /* Internal event counts (overflows, etc.), for analysis. */
  virtual void PrintDiagnostics(std::ostream &os) const { (void)os; }
};

/* A direction predictor for conditional branches (tournament or TAGE-SC-L). */
class DirectionPredictor {
 public:
  virtual ~DirectionPredictor() {}
  /* Taken or not, for the conditional branch at pc. No side effects. */
  virtual bool Predict(uint64_t pc) const = 0;
  /* Train with the outcome of the conditional branch at pc. */
  virtual void Update(uint64_t pc, bool taken) = 0;
  /* Recent conditional outcomes, newest in bit 0 (shared with the indirect
     target predictor's itp4_gh context). */
  virtual uint64_t History() const = 0;
  virtual uint64_t StorageBits() const = 0;
  virtual void PrintConfig(std::ostream &os) const = 0;
  virtual void PrintDiagnostics(std::ostream &os) const { (void)os; }
};

/* The assignment's storage budget: 12 KiB. */
static const uint64_t kStorageBudgetBits = 98304;

namespace bp {

static inline bool IsPow2(uint64_t n) { return n != 0 && (n & (n - 1)) == 0; }

/* log2 of a power of two. */
static inline unsigned Log2(uint64_t n)
{
  unsigned b = 0;
  while ((1ULL << b) < n) b++;
  return b;
}

/* Bits needed to hold the values 0..n. */
static inline unsigned BitsFor(uint64_t n)
{
  unsigned b = 0;
  while ((1ULL << b) <= n) b++;
  return b;
}

static inline uint64_t LowMask(unsigned bits)
{
  return bits >= 64 ? ~0ULL : ((1ULL << bits) - 1);
}

/* Saturating counter of `bits` bits. */
static inline uint8_t SatUpdate(uint8_t c, bool up, unsigned bits)
{
  uint8_t max = (uint8_t)((1u << bits) - 1);
  if (up) return c < max ? c + 1 : c;
  return c > 0 ? c - 1 : c;
}

/* Counter predicts "taken" when its top bit is set. */
static inline bool SatTaken(uint8_t c, unsigned bits) { return c >= (1u << (bits - 1)); }

/* Weakest "not taken" value: just below the taken threshold. */
static inline uint8_t SatWeakNotTaken(unsigned bits) { return (uint8_t)((1u << (bits - 1)) - 1); }

/*
 * Addresses are stored in fewer than 64 bits. Only the low `bits` bits are
 * kept; on prediction, the missing upper bits are taken from the address of
 * the branch being predicted. This is right whenever the branch and its
 * target share those upper bits (the same 4 GiB region, for 32 bits).
 */
static inline uint64_t Truncate(uint64_t addr, unsigned bits) { return addr & LowMask(bits); }

static inline uint64_t Restore(uint64_t pc, uint64_t stored, unsigned bits)
{
  return (pc & ~LowMask(bits)) | stored;
}

}  // namespace bp

#endif
