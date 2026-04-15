/*
 * memory_hog.c - Controlled memory pressure generator
 *
 * Improvements:
 *  - deterministic growth (MB per step)
 *  - guaranteed RSS increase (page touching)
 *  - configurable delay (ms)
 *  - never exits prematurely
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_CHUNK_MB 1
#define DEFAULT_SLEEP_MS 1000

static size_t parse_mb(const char *arg, size_t def) {
    if (!arg) return def;
    char *end;
    long val = strtol(arg, &end, 10);
    if (*end != '\0' || val <= 0) return def;
    return (size_t)val;
}

static useconds_t parse_ms(const char *arg, useconds_t def) {
    if (!arg) return def;
    char *end;
    long val = strtol(arg, &end, 10);
    if (*end != '\0' || val <= 0) return def;
    return (useconds_t)(val * 1000);
}

int main(int argc, char *argv[]) {
    size_t chunk_mb = (argc > 1) ? parse_mb(argv[1], DEFAULT_CHUNK_MB) : DEFAULT_CHUNK_MB;
    useconds_t sleep_us = (argc > 2) ? parse_ms(argv[2], DEFAULT_SLEEP_MS) : DEFAULT_SLEEP_MS * 1000;

    size_t chunk_bytes = chunk_mb * 1024 * 1024;
    size_t total_mb = 0;

    printf("memory_hog started: chunk=%zuMB sleep=%dus\n", chunk_mb, sleep_us);
    fflush(stdout);

    while (1) {
        char *mem = malloc(chunk_bytes);
        if (!mem) {
            printf("malloc failed at %zu MB\n", total_mb);
            fflush(stdout);
            while (1) sleep(1);  // stay alive for observation
        }

        /* Touch every page to ensure RSS increase */
        for (size_t i = 0; i < chunk_bytes; i += 4096) {
            mem[i] = 'A';
        }

        total_mb += chunk_mb;

        printf("allocated: %zu MB total\n", total_mb);
        fflush(stdout);

        usleep(sleep_us);
    }

    return 0;
}
