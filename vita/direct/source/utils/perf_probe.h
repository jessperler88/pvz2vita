#ifndef PVZ2_PERF_PROBE_H
#define PVZ2_PERF_PROBE_H

/* Measurement-only counters for the game's hottest imports (allocation, copies,
 * mutexes). Built only with -DPVZ2_PERF_PROBE=1; reported with the opt-in
 * 300-frame FRAME log. One call in 16 per kind is timed, so the per-call cost
 * is an estimate with the timer's own overhead subtracted. */

#include <stddef.h>
#include <stdint.h>

enum {
    PROBE_MALLOC, PROBE_FREE, PROBE_REALLOC, PROBE_CALLOC, PROBE_MEMALIGN,
    PROBE_MEMCPY, PROBE_MEMSET, PROBE_MEMMOVE, PROBE_MUTEX_LOCK, PROBE_KINDS
};

#if PVZ2_PERF_PROBE
uint64_t perf_probe_begin(unsigned kind, size_t bytes);
void perf_probe_end(unsigned kind, uint64_t start);
void perf_probe_format(char *out, size_t capacity, unsigned frames);
#define PERF_PROBE_BEGIN(kind, bytes) const uint64_t perf_probe_t0_ = perf_probe_begin((kind), (bytes))
#define PERF_PROBE_END(kind) perf_probe_end((kind), perf_probe_t0_)
#else
#define PERF_PROBE_BEGIN(kind, bytes) ((void)0)
#define PERF_PROBE_END(kind) ((void)0)
#endif

#endif
