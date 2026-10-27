#include "perf_probe.h"

#if PVZ2_PERF_PROBE
#include <stdatomic.h>
#include <stdio.h>
#include <psp2/kernel/threadmgr.h>

/* Allocation size buckets: <=16, 32, 64, 128, 256, 1K, 4K, 64K, larger. */
#define BUCKETS 9
static const size_t k_bucket_max[BUCKETS - 1] = {16, 32, 64, 128, 256, 1024, 4096, 65536};
static const char *const k_names[PROBE_KINDS] = {
    "malloc", "free", "realloc", "calloc", "memalign", "memcpy", "memset", "memmove", "lock"
};

static atomic_uint g_calls[PROBE_KINDS];
static atomic_ullong g_bytes[PROBE_KINDS];
static atomic_ullong g_sampled_us[PROBE_KINDS];
static atomic_uint g_samples[PROBE_KINDS];
static atomic_uint g_alloc_buckets[BUCKETS];

static unsigned bucket_of(size_t bytes) {
    unsigned i = 0;
    while (i < BUCKETS - 1 && bytes > k_bucket_max[i]) ++i;
    return i;
}

uint64_t perf_probe_begin(unsigned kind, size_t bytes) {
    const unsigned n = atomic_fetch_add_explicit(&g_calls[kind], 1, memory_order_relaxed);
    if (bytes) atomic_fetch_add_explicit(&g_bytes[kind], bytes, memory_order_relaxed);
    if (kind == PROBE_MALLOC || kind == PROBE_CALLOC || kind == PROBE_MEMALIGN || kind == PROBE_REALLOC)
        atomic_fetch_add_explicit(&g_alloc_buckets[bucket_of(bytes)], 1, memory_order_relaxed);
    return (n & 15u) == 0 ? sceKernelGetSystemTimeWide() : 0;
}

void perf_probe_end(unsigned kind, uint64_t start) {
    if (!start) return;
    const uint64_t end = sceKernelGetSystemTimeWide();
    atomic_fetch_add_explicit(&g_sampled_us[kind], end - start, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_samples[kind], 1, memory_order_relaxed);
}

/* Cost of one begin/end timer pair, in nanoseconds, measured on this thread. */
static unsigned timer_overhead_ns(void) {
    uint64_t total = 0;
    for (int i = 0; i < 256; ++i) {
        const uint64_t a = sceKernelGetSystemTimeWide();
        total += sceKernelGetSystemTimeWide() - a;
    }
    return (unsigned)(total * 1000u / 256u);
}

void perf_probe_format(char *out, size_t capacity, unsigned frames) {
    if (!frames) frames = 1;
    const unsigned overhead = timer_overhead_ns();
    size_t used = (size_t)snprintf(out, capacity, "timer_ns=%u per_frame:", overhead);
    uint64_t total_est_us = 0;
    for (unsigned k = 0; k < PROBE_KINDS && used < capacity; ++k) {
        const unsigned calls = atomic_exchange_explicit(&g_calls[k], 0, memory_order_relaxed);
        const uint64_t bytes = atomic_exchange_explicit(&g_bytes[k], 0, memory_order_relaxed);
        const uint64_t us = atomic_exchange_explicit(&g_sampled_us[k], 0, memory_order_relaxed);
        const unsigned samples = atomic_exchange_explicit(&g_samples[k], 0, memory_order_relaxed);
        if (!calls) continue;
        /* Mean sampled cost per call minus the timer pair's own cost. */
        int64_t ns = samples ? (int64_t)(us * 1000u / samples) - (int64_t)overhead : 0;
        if (ns < 0) ns = 0;
        const uint64_t est_us = (uint64_t)ns * calls / 1000u;
        total_est_us += est_us;
        used += (size_t)snprintf(out + used, capacity - used, " %s=%u/%uns/%uB/%uus",
                                 k_names[k], calls / frames, (unsigned)ns,
                                 (unsigned)(bytes / frames), (unsigned)(est_us / frames));
    }
    if (used < capacity)
        used += (size_t)snprintf(out + used, capacity - used, " total_est_us=%u sizes:",
                                 (unsigned)(total_est_us / frames));
    for (unsigned b = 0; b < BUCKETS && used < capacity; ++b)
        used += (size_t)snprintf(out + used, capacity - used, "%s%u", b ? "," : "",
                                 atomic_exchange_explicit(&g_alloc_buckets[b], 0, memory_order_relaxed) / frames);
}
#endif
