/*
 * t_indirect.c -- directed test: indirect calls and an indirect jump.
 *
 * dispatch() calls through a function-pointer table, so one call instruction
 * rotates among three targets (add1, dbl, neg). sw() is a dense switch, which
 * gcc compiles to a jump table: one indirect jump with a different target per
 * case. See EXPECTED.md for the event counts to check.
 */
#include <stdio.h>

typedef int (*op_t)(int);

__attribute__((noinline)) int add1(int x) { return x + 1; }
__attribute__((noinline)) int dbl(int x)  { return x * 2; }
__attribute__((noinline)) int neg(int x)  { return -x; }

op_t ops[3] = { add1, dbl, neg };

__attribute__((noinline)) int dispatch(int n)
{
    int acc = 0;
    for (int i = 0; i < n; i++)
        acc += ops[i % 3](i);
    return acc;
}

__attribute__((noinline)) int sw(int k)
{
    switch (k) {
    case 0: return 10;
    case 1: return 11;
    case 2: return 12;
    case 3: return 13;
    case 4: return 14;
    case 5: return 15;
    default: return -1;
    }
}

int main(void)
{
    int a = dispatch(6);
    int b = 0;
    for (int k = 0; k < 6; k++)
        b += sw(k);
    printf("dispatch=%d sw=%d\n", a, b);
    return 0;
}
