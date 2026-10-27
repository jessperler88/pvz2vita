/* Measurement-only timing of libPVZ2's object lookup (sub_b8e2a0).
 *
 * The function resolves a key to a weak object pointer through two chained
 * unordered_map lookups. It takes (out, registry, key) in r0-r2, returns its
 * 8-byte result through out, and is not recursive, so a plain C wrapper can
 * forward it. Built only with -DPVZ2_LOOKUP_PROBE=1; reported with the opt-in
 * 300-frame log. One call in 16 is timed and attributed to its caller. */

#include "lookup_probe.h"

#if PVZ2_LOOKUP_PROBE
#include <stdatomic.h>
#include <stdio.h>
#include <psp2/kernel/threadmgr.h>

typedef void (*lookup_fn)(void *, void *, const void *, uint32_t);
static lookup_fn g_original;
static uintptr_t g_text_base, g_text_size;

static atomic_uint g_calls, g_misses, g_samples;
static atomic_ullong g_sampled_us;

/* Sampled calls per call site (open addressing on the return address). */
#define SITES 512
static atomic_uint g_site_lr[SITES];
static atomic_uint g_site_samples[SITES];
static atomic_ullong g_site_us[SITES];
static atomic_uint g_site_overflow;

static void record_site(uint32_t lr, uint32_t us) {
    unsigned slot = (lr >> 2) * 2654435761u >> 23; /* 9 bits */
    for (unsigned probe = 0; probe < 16; ++probe, slot = (slot + 1) & (SITES - 1)) {
        uint32_t seen = atomic_load_explicit(&g_site_lr[slot], memory_order_relaxed);
        if (!seen) {
            uint32_t expected = 0;
            if (atomic_compare_exchange_strong(&g_site_lr[slot], &expected, lr)) seen = lr;
            else seen = expected;
        }
        if (seen == lr) {
            atomic_fetch_add_explicit(&g_site_samples[slot], 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&g_site_us[slot], us, memory_order_relaxed);
            return;
        }
    }
    atomic_fetch_add_explicit(&g_site_overflow, 1, memory_order_relaxed);
}

static void lookup_wrapper(void *out, void *registry, const void *key, uint32_t r3) {
    const unsigned n = atomic_fetch_add_explicit(&g_calls, 1, memory_order_relaxed);
    if (n & 15u) {
        g_original(out, registry, key, r3);
    } else {
        const uint32_t lr = (uint32_t)(uintptr_t)__builtin_return_address(0);
        const uint64_t t0 = sceKernelGetSystemTimeWide();
        g_original(out, registry, key, r3);
        const uint32_t us = (uint32_t)(sceKernelGetSystemTimeWide() - t0);
        atomic_fetch_add_explicit(&g_sampled_us, us, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_samples, 1, memory_order_relaxed);
        record_site(lr, us);
    }
    const uint32_t *result = out;
    if (!result[0] && !result[1]) atomic_fetch_add_explicit(&g_misses, 1, memory_order_relaxed);
}

void *lookup_probe_wrapper(void) { return (void *)lookup_wrapper; }

void lookup_probe_set_original(void *original, uintptr_t text_base, uintptr_t text_size) {
    g_original = (lookup_fn)original;
    g_text_base = text_base;
    g_text_size = text_size;
}

static unsigned timer_overhead_ns(void) {
    uint64_t total = 0;
    for (int i = 0; i < 256; ++i) {
        const uint64_t a = sceKernelGetSystemTimeWide();
        total += sceKernelGetSystemTimeWide() - a;
    }
    return (unsigned)(total * 1000u / 256u);
}

void lookup_probe_format(char *out, size_t capacity, unsigned frames, unsigned game_us) {
    if (!frames) frames = 1;
    if (!g_original) {
        snprintf(out, capacity, "not installed");
        return;
    }
    const unsigned overhead = timer_overhead_ns();
    const unsigned calls = atomic_exchange_explicit(&g_calls, 0, memory_order_relaxed);
    const unsigned misses = atomic_exchange_explicit(&g_misses, 0, memory_order_relaxed);
    const uint64_t us = atomic_exchange_explicit(&g_sampled_us, 0, memory_order_relaxed);
    const unsigned samples = atomic_exchange_explicit(&g_samples, 0, memory_order_relaxed);
    int64_t ns = samples ? (int64_t)(us * 1000u / samples) - (int64_t)overhead : 0;
    if (ns < 0) ns = 0;
    const uint64_t est_us_frame = (uint64_t)ns * calls / 1000u / frames;
    const unsigned pct_x10 = game_us ? (unsigned)(est_us_frame * 1000u / game_us) : 0;
    size_t used = (size_t)snprintf(out, capacity,
        "calls/frame=%u misses/frame=%u ns/call=%u est_us/frame=%u game_us=%u share=%u.%u%% timer_ns=%u samples=%u top_callers:",
        calls / frames, misses / frames, (unsigned)ns, (unsigned)est_us_frame, game_us,
        pct_x10 / 10u, pct_x10 % 10u, overhead, samples);

    /* Report the ten busiest call sites by sampled time, then reset all. */
    static uint32_t lr_snap[SITES], n_snap[SITES]; /* render thread only */
    static uint64_t us_snap[SITES];
    for (unsigned i = 0; i < SITES; ++i) {
        lr_snap[i] = atomic_exchange_explicit(&g_site_lr[i], 0, memory_order_relaxed);
        n_snap[i] = atomic_exchange_explicit(&g_site_samples[i], 0, memory_order_relaxed);
        us_snap[i] = atomic_exchange_explicit(&g_site_us[i], 0, memory_order_relaxed);
    }
    unsigned sites_seen = 0;
    for (unsigned i = 0; i < SITES; ++i) sites_seen += lr_snap[i] != 0;
    for (unsigned rank = 0; rank < 10 && used < capacity; ++rank) {
        unsigned best = SITES;
        for (unsigned i = 0; i < SITES; ++i)
            if (lr_snap[i] && (best == SITES || us_snap[i] > us_snap[best] ||
                               (us_snap[i] == us_snap[best] && n_snap[i] > n_snap[best])))
                best = i;
        if (best == SITES) break;
        const uint32_t lr = lr_snap[best];
        const unsigned off = (lr >= g_text_base && lr < g_text_base + g_text_size)
            ? (unsigned)(lr - g_text_base - 4) : 0; /* call instruction offset */
        const unsigned share = samples ? n_snap[best] * 100u / samples : 0;
        int64_t site_ns = (int64_t)us_snap[best] * 1000 - (int64_t)n_snap[best] * overhead;
        if (site_ns < 0) site_ns = 0;
        used += (size_t)snprintf(out + used, capacity - used, " 0x%x:%u%%calls/%uus",
                                 off, share, (unsigned)(site_ns * 16 / 1000 / frames));
        lr_snap[best] = 0;
    }
    if (used < capacity)
        snprintf(out + used, capacity - used, " sites=%u overflow=%u", sites_seen,
                 atomic_exchange_explicit(&g_site_overflow, 0, memory_order_relaxed));
}
#endif
