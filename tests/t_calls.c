/*
 * t_calls.c -- directed test: nested calls and returns, plus recursion.
 *
 * top() -> mid() -> leaf() nests three deep, twice. fact(5) recurses five
 * deep. See EXPECTED.md for the event counts to check.
 */
#include <stdio.h>

__attribute__((noinline)) int leaf(int x) { return x + 1; }
__attribute__((noinline)) int mid(int x)  { return leaf(x) * 2; }
__attribute__((noinline)) int top(int x)  { return mid(x) + mid(x + 1); }

__attribute__((noinline)) int fact(int n)
{
    if (n <= 1)
        return 1;
    return n * fact(n - 1);
}

int main(void)
{
    int a = top(3);
    int b = fact(5);
    printf("top=%d fact=%d\n", a, b);
    return 0;
}
