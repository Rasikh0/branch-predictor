/*
 * recurse.c -- workload (iii): recursion with nested calls and returns.
 *
 * Builds a binary search tree of pseudo-random keys by recursive insertion,
 * walks it recursively (sum and height), and computes Fibonacci numbers by
 * naive recursion. A random tree of this size is far more than 16 levels
 * deep, so the 16-entry return address stack overflows in places.
 *
 * Usage: ./recurse [nodes] [fib_n]     (defaults 50000, 24)
 */
#include <stdio.h>
#include <stdlib.h>

#include "rng.h"

#define MAXNODES (1 << 20)

typedef struct {
    int key;
    int left, right;   /* indices into pool, -1 for none */
} Node;

static Node pool[MAXNODES];
static int used;

__attribute__((noinline)) static int insert(int root, int key)
{
    if (root < 0) {
        pool[used].key = key;
        pool[used].left = pool[used].right = -1;
        return used++;
    }
    if (key < pool[root].key)
        pool[root].left = insert(pool[root].left, key);
    else
        pool[root].right = insert(pool[root].right, key);
    return root;
}

__attribute__((noinline)) static long sum(int root)
{
    if (root < 0)
        return 0;
    return sum(pool[root].left) + pool[root].key + sum(pool[root].right);
}

__attribute__((noinline)) static int height(int root)
{
    if (root < 0)
        return 0;
    int l = height(pool[root].left);
    int r = height(pool[root].right);
    return 1 + (l > r ? l : r);
}

__attribute__((noinline)) static long fib(int n)
{
    if (n < 2)
        return n;
    return fib(n - 1) + fib(n - 2);
}

int main(int argc, char **argv)
{
    int nodes = argc > 1 ? atoi(argv[1]) : 50000;
    int fn = argc > 2 ? atoi(argv[2]) : 24;
    if (nodes < 1 || nodes > MAXNODES)
        nodes = 50000;

    int root = -1;
    for (int i = 0; i < nodes; i++)
        root = insert(root, (int)(rng_next() & 0x3fffffff));

    long s = sum(root);
    int h = height(root);
    long f = fib(fn);
    printf("recurse: nodes=%d sum=%ld height=%d fib(%d)=%ld\n", nodes, s, h, fn, f);
    return 0;
}
