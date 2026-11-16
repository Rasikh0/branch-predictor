/*
 * branch_event.h -- control-flow event definitions for HW3.
 *
 * This header does not depend on Pin, so the predictor code that consumes
 * these events can also be compiled and tested outside Pin.
 */
#ifndef HW3_BRANCH_EVENT_H
#define HW3_BRANCH_EVENT_H

#include <stdint.h>
#include <string>

/* Control-flow types. There are six, so a type fits in the BTB's 3-bit field. */
enum BranchType {
  BR_COND          = 0,  /* conditional direct branch: jcc, jrcxz, loop */
  BR_JMP_DIRECT    = 1,  /* unconditional jump, target encoded in the instruction */
  BR_JMP_INDIRECT  = 2,  /* unconditional jump through a register or memory */
  BR_CALL_DIRECT   = 3,
  BR_CALL_INDIRECT = 4,
  BR_RET           = 5,
  BR_NUM_TYPES     = 6
};

static inline const char *BranchTypeName(int t)
{
  static const char *const kNames[BR_NUM_TYPES] = {
    "cond", "jmp_direct", "jmp_indirect", "call_direct", "call_indirect", "ret"
  };
  return (t >= 0 && t < BR_NUM_TYPES) ? kNames[t] : "?";
}

/*
 * Facts about one static control-flow instruction. Computed once, when Pin
 * first instruments the instruction, and shared by all its dynamic events.
 */
struct StaticBranchInfo {
  uint64_t    pc            = 0;  /* address of the instruction */
  uint32_t    size          = 0;  /* length in bytes (x86-64 is variable length) */
  BranchType  type          = BR_COND;
  uint64_t    direct_target = 0;  /* target encoded in the instruction; 0 if indirect or ret */
  uint64_t    fallthrough   = 0;  /* pc + size for cond branches and calls (for calls this is
                                     the return address); 0 for jumps and returns */
  bool        in_main       = false;  /* in the main executable (not loader/libraries) */
  uint32_t    id            = 0;  /* 0, 1, 2, ... in order of first execution; indexes
                                     per-branch statistics */
  std::string image;          /* file name of the image containing the instruction */
  std::string routine;        /* enclosing function name, if symbols are available */
  std::string disasm;         /* disassembly, AT&T syntax to match objdump */
};

/* One dynamic control-flow event: what the predictor sees, in program order. */
struct BranchEvent {
  uint64_t                seq;         /* event sequence number, starting at 0 */
  uint64_t                inst_count;  /* cumulative instructions executed, including this one */
  const StaticBranchInfo *info;
  bool                    taken;       /* always true for jumps, calls and returns */
  uint64_t                next_pc;     /* address actually executed next */
};

#endif
