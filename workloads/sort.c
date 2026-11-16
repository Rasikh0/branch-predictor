/*
 * sort.c -- workload (ii): sorting and searching with data-dependent branches.
 *
 * Fills an array with pseudo-random keys, sorts it with quicksort, then runs
 * binary searches for keys that are half present and half random. Whether
 * each comparison goes one way or the other depends on the data.
 *
 * Usage: ./sort [n]           (default 100000)
 */
#include <stdio.h>
#include <stdlib.h>

#include "rng.h"

#define MAXN (1 << 20)

static int v[MAXN];

__attribute__((noinline)) static void quicksort(int *a, long lo, long hi)
{
    if (lo >= hi)
        return;
    int pivot = a[lo + (hi - lo) / 2];
    long i = lo, j = hi;
    while (i <= j) {
        while (a[i] < pivot)
            i++;
        while (a[j] > pivot)
            j--;
        if (i <= j) {
            int t = a[i];
            a[i] = a[j];
            a[j] = t;
            i++;
            j--;
        }
    }
    quicksort(a, lo, j);
    quicksort(a, i, hi);
}

__attribute__((noinline)) static long search(const int *a, long n, int key)
{
    long lo = 0, hi = n - 1;
    while (lo <= hi) {
        long mid = lo + (hi - lo) / 2;
        if (a[mid] == key)
            return mid;
        if (a[mid] < key)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

int main(int argc, char **argv)
{
    long n = argc > 1 ? atol(argv[1]) : 100000;
    if (n < 1 || n > MAXN)
        n = 100000;

    for (long i = 0; i < n; i++)
        v[i] = (int)(rng_next() & 0x3fffffff);

    quicksort(v, 0, n - 1);

    long found = 0;
    for (long i = 0; i < n; i++) {
        uint64_t r = rng_next();
        int key = (r & 1) ? v[(r >> 1) % n] : (int)((r >> 1) & 0x3fffffff);
        if (search(v, n, key) >= 0)
            found++;
    }

    long sorted = 1;
    for (long i = 1; i < n; i++)
        if (v[i - 1] > v[i])
            sorted = 0;
    printf("sort: n=%ld sorted=%ld found=%ld\n", n, sorted, found);
    return 0;
}
