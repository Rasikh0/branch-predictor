/*
 * rng.h -- deterministic pseudo-random numbers for the workloads.
 *
 * A fixed-seed xorshift generator, so every run sees identical data and
 * therefore identical branch behavior. It lives in the program itself (not
 * libc's rand()), so generating data doesn't add library calls.
 */
#ifndef HW3_RNG_H
#define HW3_RNG_H

#include <stdint.h>

static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;

static inline uint64_t rng_next(void)
{
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return rng_state = x;
}

#endif
