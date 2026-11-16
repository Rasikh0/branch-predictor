/*
 * t_patterns.c -- directed test for the Section 9 bonus (TAGE-SC-L).
 *
 * Each function holds one branch pattern, so the per-branch profile shows
 * which predictor handles which pattern:
 *   loop_50    a loop of 50 iterations: the exit is beyond the tournament's
 *              10/12-outcome histories but within TAGE's 77/160
 *   loop_300   a loop of 300 iterations: beyond TAGE's longest history (160);
 *              only the loop predictor can count it
 *   corr_30    branch Y repeats random branch X's outcome, with a 30-iteration
 *              loop in between (about 31 branches): needs a history of 31+
 *   biased_85  random, taken 85% of the time: no history helps; the best
 *              possible is about 15% wrong
 *   random_50  a coin flip: about 50% wrong for every predictor
 * Compiled with -O2 like the workloads; `sink` is volatile so the branches
 * stay real branches (checked with objdump).
 *
 * Usage: ./t_patterns [calls]      (default 20000)
 */
#include <stdio.h>
#include <stdlib.h>

#include "../workloads/rng.h"

volatile long sink;

__attribute__((noinline)) void loop_50(void)
{
    for (int i = 0; i < 50; i++)
        sink += i;
}

__attribute__((noinline)) void loop_300(void)
{
    for (int i = 0; i < 300; i++)
        sink += i;
}

__attribute__((noinline)) void corr_30(int x)
{
    if (x)                           /* branch X: random */
        sink += 1;
    for (int i = 0; i < 30; i++)     /* 30 loop branches in between */
        sink += i;
    if (x)                           /* branch Y: same outcome as X */
        sink += 2;
}

__attribute__((noinline)) void biased_85(int r)
{
    if (r < 85)
        sink += 3;
}

__attribute__((noinline)) void random_50(int r)
{
    if (r)
        sink += 4;
}

int main(int argc, char **argv)
{
    int calls = argc > 1 ? atoi(argv[1]) : 20000;
    for (int k = 0; k < calls; k++) {
        uint64_t r = rng_next();
        loop_50();
        loop_300();
        corr_30((int)(r & 1));
        biased_85((int)((r >> 8) % 100));
        random_50((int)((r >> 20) & 1));
    }
    printf("t_patterns: calls=%d sink=%ld\n", calls, sink);
    return 0;
}
