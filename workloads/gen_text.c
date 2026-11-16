/*
 * gen_text.c -- generates the input file for the gzip workload.
 *
 * Writes pseudo-random English-like text (made-up words, punctuation, line
 * breaks) from a fixed seed, so the input, and hence gzip's run, is
 * identical every time. Common words are much more frequent than rare ones,
 * as in real text, so the output compresses like text.
 *
 * Usage: ./gen_text BYTES > file
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rng.h"

#define NWORDS 2000

static const char *syll[] = {
    "ka", "re", "mi", "to", "sa", "ne", "lo", "vi", "da", "pe", "ru", "shi",
    "an", "el", "or", "un", "ti", "go", "ba", "fe", "ch", "th", "st", "qu"
};

int main(int argc, char **argv)
{
    long bytes = argc > 1 ? atol(argv[1]) : 1000000;
    static char words[NWORDS][16];
    int nsyll = sizeof syll / sizeof syll[0];

    for (int w = 0; w < NWORDS; w++) {
        int parts = 1 + (int)(rng_next() % 3);
        words[w][0] = '\0';
        for (int p = 0; p < parts; p++)
            strcat(words[w], syll[rng_next() % nsyll]);
    }

    long out = 0;
    int col = 0;
    while (out < bytes) {
        /* Product of two uniforms: small indices (common words) dominate. */
        uint64_t r1 = rng_next() % NWORDS, r2 = rng_next() % NWORDS;
        const char *w = words[(r1 * r2) / NWORDS];
        int len = (int)strlen(w);
        out += fputs(w, stdout) >= 0 ? len : 0;
        uint64_t p = rng_next() % 20;
        const char *sep = p == 0 ? ". " : p == 1 ? ", " : " ";
        fputs(sep, stdout);
        out += (long)strlen(sep);
        col += len + (int)strlen(sep);
        if (col > 70) {
            fputc('\n', stdout);
            out++;
            col = 0;
        }
    }
    return 0;
}
