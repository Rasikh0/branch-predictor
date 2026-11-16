/*
 * t_cond.c -- directed test: conditional branches, taken and not taken.
 *
 * cond_test() runs a 10-iteration loop with an if inside it that is true
 * for i = 0, 3, 6, 9. See EXPECTED.md for the event counts to check.
 */
#include <stdio.h>

volatile int sink;   /* volatile so the compiler keeps every access */

__attribute__((noinline)) void cond_test(void)
{
    for (int i = 0; i < 10; i++) {
        if (i % 3 == 0)
            sink++;
    }
}

int main(void)
{
    cond_test();
    printf("sink=%d\n", sink);
    return 0;
}
