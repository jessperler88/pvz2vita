/* Faster 32-bit division/modulo for the game's helper imports.
 *
 * Cortex-A9 has no hardware divide, so every `/` and `%` in libPVZ2 calls a
 * software routine. Most calls come from hash-table lookups that divide by the
 * same bucket count again and again. For a divisor seen before, a cached
 * multiplier turns the division into one 64-bit multiply and a few shifts
 * (Granlund-Montgomery, "Division by Invariant Integers using Multiplication").
 *
 * Every fast result is checked: q*d <= n and n - q*d < d. That quotient and
 * remainder are unique, so a stale or torn cache entry from another thread can
 * only cause a fallback to the normal division, never a wrong answer. Division
 * by zero keeps the original helper's behaviour. */
#include "reimpl/fastdiv.h"

#include <stdint.h>
#include <string.h>
#ifndef PVZ2_FASTDIV_HOST_TEST
#include <kubridge.h>
#include "utils/telemetry.h"
#endif

#define CACHE_BITS 6
#define CACHE_SIZE (1u << CACHE_BITS)

typedef struct {
    volatile uint32_t divisor;
    volatile uint32_t magic;
} Entry;

static Entry g_cache[CACHE_SIZE];
static uint32_t (*g_orig_umod)(uint32_t, uint32_t);
static int32_t (*g_orig_mod)(int32_t, int32_t);
static uint32_t (*g_orig_udiv)(uint32_t, uint32_t);
static int32_t (*g_orig_idiv)(int32_t, int32_t);

#if PVZ2_FASTDIV_STATS
volatile uint32_t pvz2_fastdiv_calls, pvz2_fastdiv_misses;
#define STAT(x) ((x)++)
#else
#define STAT(x) ((void)0)
#endif

static inline unsigned slot_of(uint32_t d) {
    return (d * 2654435761u) >> (32 - CACHE_BITS);
}

/* l = ceil(log2 d); magic = floor(2^32 * (2^l - d) / d) + 1. */
static uint32_t magic_for(uint32_t d) {
    const unsigned l = d <= 1 ? 0 : 32u - (unsigned)__builtin_clz(d - 1);
    const uint64_t num = ((1ull << l) - d) << 32; /* (2^l - d) < 2^32 for l <= 32 */
    return (uint32_t)(num / d) + 1u;
}

static inline uint32_t quotient(uint32_t n, uint32_t d, uint32_t magic) {
    const unsigned l = d <= 1 ? 0 : 32u - (unsigned)__builtin_clz(d - 1);
    const uint32_t t = (uint32_t)(((uint64_t)magic * n) >> 32);
    const unsigned s1 = l ? 1 : 0, s2 = l ? l - 1 : 0;
    return (t + ((n - t) >> s1)) >> s2;
}

/* Returns 1 and sets *q/*r when the cached fast path produced a checked result. */
static inline int fast_divmod(uint32_t n, uint32_t d, uint32_t *q, uint32_t *r) {
    Entry *e = &g_cache[slot_of(d)];
    STAT(pvz2_fastdiv_calls);
    uint32_t magic;
    if (e->divisor == d) {
        magic = e->magic;
    } else {
        STAT(pvz2_fastdiv_misses);
        magic = magic_for(d);
        e->divisor = 0;     /* readers of the old divisor fail the check below */
        e->magic = magic;
        e->divisor = d;
    }
    const uint32_t qq = quotient(n, d, magic);
    const uint64_t product = (uint64_t)qq * d;
    if (product > n) return 0;
    const uint32_t rr = n - (uint32_t)product;
    if (rr >= d) return 0;
    *q = qq;
    *r = rr;
    return 1;
}

uint32_t pvz2_umodsi3(uint32_t n, uint32_t d) {
    uint32_t q, r;
    if (d && fast_divmod(n, d, &q, &r)) return r;
    return g_orig_umod(n, d);
}

int32_t pvz2_modsi3(int32_t n, int32_t d) {
    uint32_t q, r;
    if (d) {
        const uint32_t un = n < 0 ? 0u - (uint32_t)n : (uint32_t)n;
        const uint32_t ud = d < 0 ? 0u - (uint32_t)d : (uint32_t)d;
        if (fast_divmod(un, ud, &q, &r)) return n < 0 ? (int32_t)(0u - r) : (int32_t)r;
    }
    return g_orig_mod(n, d);
}

uint32_t pvz2_aeabi_uidiv(uint32_t n, uint32_t d) {
    uint32_t q, r;
    if (d && fast_divmod(n, d, &q, &r)) return q;
    return g_orig_udiv(n, d);
}

int32_t pvz2_aeabi_idiv(int32_t n, int32_t d) {
    uint32_t q, r;
    if (d) {
        const uint32_t un = n < 0 ? 0u - (uint32_t)n : (uint32_t)n;
        const uint32_t ud = d < 0 ? 0u - (uint32_t)d : (uint32_t)d;
        if (fast_divmod(un, ud, &q, &r)) return (n < 0) != (d < 0) ? (int32_t)(0u - q) : (int32_t)q;
    }
    return g_orig_idiv(n, d);
}

void pvz2_fastdiv_set_fallbacks(uint32_t (*umod)(uint32_t, uint32_t), int32_t (*mod)(int32_t, int32_t),
                                uint32_t (*udiv)(uint32_t, uint32_t), int32_t (*idiv)(int32_t, int32_t)) {
    g_orig_umod = umod;
    g_orig_mod = mod;
    g_orig_udiv = udiv;
    g_orig_idiv = idiv;
}

#ifndef PVZ2_FASTDIV_HOST_TEST
/* libPVZ2 defines its own copies of these helpers, so so_relocate binds the
 * game's calls straight to them. Keep those originals as the fallback and
 * repoint only the game's PLT/GOT slots for the four helpers. */
int pvz2_fastdiv_install(so_module *mod) {
    static const struct { const char *name; uintptr_t fn; } k[] = {
        {"__umodsi3", (uintptr_t)pvz2_umodsi3}, {"__modsi3", (uintptr_t)pvz2_modsi3},
        {"__aeabi_uidiv", (uintptr_t)pvz2_aeabi_uidiv}, {"__aeabi_idiv", (uintptr_t)pvz2_aeabi_idiv},
    };
    const uintptr_t umod = so_symbol(mod, "__umodsi3"), smod = so_symbol(mod, "__modsi3");
    const uintptr_t udiv = so_symbol(mod, "__aeabi_uidiv"), idiv = so_symbol(mod, "__aeabi_idiv");
    if (!umod || !smod || !udiv || !idiv) return -1;
    pvz2_fastdiv_set_fallbacks((uint32_t (*)(uint32_t, uint32_t))umod, (int32_t (*)(int32_t, int32_t))smod,
                               (uint32_t (*)(uint32_t, uint32_t))udiv, (int32_t (*)(int32_t, int32_t))idiv);
    unsigned patched[4] = {0};
    for (int i = 0; i < mod->num_reldyn + mod->num_relplt; i++) {
        Elf32_Rel *rel = i < mod->num_reldyn ? &mod->reldyn[i] : &mod->relplt[i - mod->num_reldyn];
        const int type = ELF32_R_TYPE(rel->r_info);
        if (type != R_ARM_JUMP_SLOT && type != R_ARM_GLOB_DAT) continue;
        const char *name = mod->dynstr + mod->dynsym[ELF32_R_SYM(rel->r_info)].st_name;
        for (unsigned j = 0; j < 4; ++j) {
            if (strcmp(name, k[j].name) != 0) continue;
            uintptr_t *slot = (uintptr_t *)(mod->text_base + rel->r_offset);
            kuKernelCpuUnrestrictedMemcpy(slot, &k[j].fn, sizeof(uintptr_t));
            ++patched[j];

        }
    }
    telemetry_log("PATCH", "fast division slots umod=%u mod=%u uidiv=%u idiv=%u",
                  patched[0], patched[1], patched[2], patched[3]);
    return 0;
}



#if PVZ2_FASTDIV_STATS
#include <psp2/kernel/threadmgr.h>
__attribute__((noinline)) static uint32_t bench_nop(uint32_t n, uint32_t d) { return n ^ d; }

/* Time the game's original __umodsi3 against the cached version on
 * hash-table-like inputs, through a function pointer like the game's PLT. */
static uint32_t bench_one(uint32_t (*volatile fn)(uint32_t, uint32_t), unsigned iters, uint32_t *sink) {
    static const uint32_t divisors[8] = {97, 389, 1543, 6151, 24593, 98317, 13, 769};
    uint32_t x = 2463534242u, acc = 0;
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    for (unsigned i = 0; i < iters; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        acc += fn(x, divisors[(i >> 4) & 7]); /* bursts of the same divisor */
    }
    *sink += acc;
    return (uint32_t)(sceKernelGetSystemTimeWide() - t0);
}

void pvz2_fastdiv_benchmark(void) {
    const unsigned iters = 200000;
    uint32_t sink = 0;
    const uint32_t base = bench_one(bench_nop, iters, &sink);
    const uint32_t orig = bench_one(g_orig_umod, iters, &sink);
    const uint32_t fast = bench_one(pvz2_umodsi3, iters, &sink);
    pvz2_fastdiv_calls = pvz2_fastdiv_misses = 0;
    /* ns per call, net of loop and call overhead (base). */
    telemetry_log("DIVBENCH", "iters=%u loop_us=%u orig_us=%u fast_us=%u => orig=%uns fast=%uns per call (sink=%u)",
                  iters, (unsigned)base, (unsigned)orig, (unsigned)fast,
                  (unsigned)((uint64_t)(orig > base ? orig - base : 0) * 1000u / iters),
                  (unsigned)((uint64_t)(fast > base ? fast - base : 0) * 1000u / iters), (unsigned)sink);
}
#endif
#endif
