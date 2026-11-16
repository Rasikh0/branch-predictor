/*
 * loop.c -- workload (i): loops with predictable branches.
 *
 * Integer matrix multiply C = A x B (N x N), repeated REPS times. Almost
 * every branch is a loop back edge with a fixed trip count.
 *
 * Usage: ./loop [reps]        (default 2)
 */
#include <stdio.h>
#include <stdlib.h>

#define N 128

static int A[N][N], B[N][N], C[N][N];

int main(int argc, char **argv)
{
    int reps = argc > 1 ? atoi(argv[1]) : 2;

    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            A[i][j] = i + j;
            B[i][j] = i - j;
        }

    for (int r = 0; r < reps; r++)
        for (int i = 0; i < N; i++)
            for (int j = 0; j < N; j++) {
                int s = 0;
                for (int k = 0; k < N; k++)
                    s += A[i][k] * B[k][j];
                C[i][j] = s + r;
            }

    long sum = 0;
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            sum += C[i][j];
    printf("loop: N=%d reps=%d checksum=%ld\n", N, reps, sum);
    return 0;
}
