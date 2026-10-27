/* Faster replacement for libPVZ2's object lookup (sub_b8e2a0, PvZ2 4.5.2).
 *
 * The game resolves an 8-byte object handle thousands of times per frame
 * (16-25% of game-logic time in busy waves). A handle is either already
 * "direct" (type index, slot index, generation) or a "named" pair of interned
 * string ids (type id, object id). For a named handle the original does:
 *   1. refcount++ on both interned-string entries, plus an unused string fetch
 *   2. type id  -> type table   (unordered_map at registry+0x1c, modulo hash)
 *   3. object id -> 12-byte key (unordered_map at type+0xf0, modulo hash)
 *   4. 12-byte key -> stored handle (std::map at type+0xe8)
 *   5. check the stored handle against the type's slot table, pack the result
 *   6. refcount-- on both entries (a count reaching zero frees the entry)
 *
 * This version does the same reads without the extra calls. Step 1/6 cancel
 * out unless a count starts at zero, so that case (and anything else unusual:
 * a named stored handle, an invalid slot the original would dereference, a
 * result that would need the named-handle assign) falls back to the original.
 * The fast path never writes game memory except *out. */
#include "reimpl/lookup_fast.h"

#include <stdint.h>
#include <string.h>

/* Interned-string pool singleton pointer (written by 0xb92c04). */
#define POOL_VAR_OFFSET 0x01179c28u

static uintptr_t g_text_base;
static lookup_fast_fn g_original;

/* ---- modulo by a bucket count, with a tiny reciprocal cache ---- */
typedef struct { volatile uint32_t d, magic, shift; } DivEntry;
static DivEntry g_div[8];

static uint32_t mod_slow(uint32_t n, uint32_t d) { return n % d; }

static inline uint32_t bucket_of(uint32_t h, uint32_t n) {
    const uint32_t mask = n - 1;
    if (!(n & mask)) return h & mask;
    DivEntry *e = &g_div[(n * 2654435761u) >> 29];
    uint32_t magic, l;
    if (e->d == n) {
        magic = e->magic;
        l = e->shift;
    } else {
        l = 32u - (unsigned)__builtin_clz(n - 1); /* n >= 3 here */
        magic = (uint32_t)((((1ull << l) - n) << 32) / n) + 1u;
        e->d = 0;
        e->magic = magic;
        e->shift = l;
        e->d = n;
    }
    const uint32_t t = (uint32_t)(((uint64_t)magic * h) >> 32);
    const uint32_t q = (t + ((h - t) >> 1)) >> (l - 1);
    const uint64_t p = (uint64_t)q * n;
    const uint32_t r = h - (uint32_t)p;
    /* A torn cache entry (another thread) only causes the slow path. */
    if (p > h || r >= n) return mod_slow(h, n);
    return r;
}

/* libc++ unordered_map<uint32_t, V> find: node = {next, hash, key, value...}. */
static inline const uint32_t *hash_find(const uint32_t *buckets, uint32_t n, uint32_t key) {
    if (!n) return NULL;
    const uint32_t b = bucket_of(key, n);
    const uint32_t *node = (const uint32_t *)buckets[b];
    if (!node) return NULL;
    for (node = (const uint32_t *)node[0]; node; node = (const uint32_t *)node[0]) {
        if (bucket_of(node[1], n) != b) return NULL;
        if (node[2] == key) return node;
    }
    return NULL;
}

static inline int key_less(const uint32_t *a, const uint32_t *b) {
    if (a[0] != b[0]) return a[0] < b[0];
    if (a[1] != b[1]) return a[1] < b[1];
    return a[2] < b[2];
}

enum { FAST_DONE = 1, FAST_FALLBACK = 0 };

#if PVZ2_LOOKUP_AB
volatile uint32_t lookup_fast_stat[LOOKUP_STAT_COUNT];
#define STAT(i) (lookup_fast_stat[(i)]++)
#else
#define STAT(i) ((void)0)
#endif

static inline int refcount_ok(const uint32_t *entries, uint32_t id) {
    if (!id) return 1;
    const uint8_t *entry = (const uint8_t *)entries[id - 1];
    return *(const uint32_t *)(entry + 24) != 0;
}

/* Returns FAST_DONE with *out written, or FAST_FALLBACK with nothing written. */
static int lookup_fast_try(uint32_t *out, const uint8_t *reg, const uint32_t *key) {
    const uint32_t k0 = key[0], k1 = key[1];
    if (!(k1 >> 31)) { /* already direct: plain copy */
        out[0] = k0;
        out[1] = k1;
        STAT(LOOKUP_STAT_DIRECT);
        return FAST_DONE;
    }
    const uint32_t type_id = ((k0 >> 31) | (k1 << 1)) & 0x7fffffffu;
    const uint32_t obj_id = k0 & 0x7fffffffu;

    const uint8_t *pool = *(const uint8_t *const *)(g_text_base + POOL_VAR_OFFSET);
    if (!pool) { STAT(LOOKUP_STAT_FB_POOL); return FAST_FALLBACK; }
    const uint32_t *entries = *(const uint32_t *const *)(pool + 12);
    if (!refcount_ok(entries, type_id) || !refcount_ok(entries, obj_id)) {
        STAT(LOOKUP_STAT_FB_REFCOUNT);
        return FAST_FALLBACK;
    }

    /* 2. type id -> type table */
    const uint32_t *tnode = hash_find(*(const uint32_t *const *)(reg + 0x1c),
                                      *(const uint32_t *)(reg + 0x20), type_id);
    if (!tnode) goto miss;
    uint32_t index = tnode[3];
    const uint32_t *vec;
    if (index & 0x2000u) {
        index &= ~0x2000u;
        vec = (const uint32_t *)reg;
    } else {
        vec = (const uint32_t *)(reg + 12);
    }
    if (index >= (uint32_t)((int32_t)(vec[1] - vec[0]) >> 2)) goto miss;
    const uint8_t *type = (const uint8_t *)((const uint32_t *)vec[0])[index];
    if (!type) goto miss;

    /* 3. object id -> 12-byte key */
    const uint32_t *onode = hash_find(*(const uint32_t *const *)(type + 0xf0),
                                      *(const uint32_t *)(type + 0xf4), obj_id);
    if (!onode) goto miss;
    const uint32_t *k = onode + 4;

    /* 4. std::map lower_bound: node = {left, right, parent, color, key[3], ..., handle@+32} */
    const uint32_t *end = (const uint32_t *)(type + 0xe8);
    const uint32_t *it = end;
    const uint32_t *n = (const uint32_t *)end[0];
    if (!n) goto miss;
    while (n) {
        if (key_less(n + 4, k)) {
            n = (const uint32_t *)n[1];
        } else {
            it = n;
            n = (const uint32_t *)n[0];
        }
    }
    if (it == end || key_less(k, it + 4)) goto miss;

    /* 5. validate the stored handle against the slot table */
    const uint32_t h0 = it[8], h1 = it[9];
    if (h1 >> 31) { STAT(LOOKUP_STAT_FB_NAMED); return FAST_FALLBACK; }
    const uint32_t *t32 = (const uint32_t *)type;
    if (((h1 >> 17) & 0x3fffu) != t32[0]) { STAT(LOOKUP_STAT_FB_SLOT); return FAST_FALLBACK; }
    const int wide = (h1 & 0x10000u) != 0;
    const uint32_t slot_index = h0 & (wide ? 0xffffffu : 0xffffu);
    if (slot_index >= t32[2]) { STAT(LOOKUP_STAT_FB_SLOT); return FAST_FALLBACK; }
    const uint32_t *slot = (const uint32_t *)(t32[1] + slot_index * 32u);
    const uint32_t gen = wide ? (((h0 >> 24) | (h1 << 8)) & 0xffffffu) : ((h0 >> 16) | (h1 << 16));
    if (gen != slot[3]) { STAT(LOOKUP_STAT_FB_SLOT); return FAST_FALLBACK; }
    if (slot[5] != k[0] || slot[6] != k[1] || slot[7] != k[2]) goto miss;

    /* pack the direct result handle (0xb935fc) */
    const uint32_t idx = (uint32_t)((int32_t)((uintptr_t)slot - t32[1]) >> 5);
    const uint32_t g = slot[3];
    uint32_t r0, hi;
    if (!(idx >> 16)) {
        r0 = idx | (g << 16);
        hi = g >> 16;
    } else {
        r0 = idx | (g << 24);
        hi = 0x10000u | (g >> 8);
    }
    const uint32_t r1 = hi | (t32[0] << 17);
    if (r1 >> 31) { STAT(LOOKUP_STAT_FB_NAMED); return FAST_FALLBACK; }
    out[0] = r0;
    out[1] = r1;
    STAT(LOOKUP_STAT_HIT);
    return FAST_DONE;

miss:
    out[0] = 0;
    out[1] = 0;
    STAT(LOOKUP_STAT_MISS);
    return FAST_DONE;
}

#if PVZ2_LOOKUP_AB
/* Test build: 0 = original, 1 = fast, 2 = verify (fast vs original, original wins). */
volatile uint32_t lookup_fast_mode;
static volatile uint32_t g_mismatch_log[8][8];
static volatile uint32_t g_mismatch_logged;

static void lookup_entry(uint32_t *out, const uint8_t *reg, const uint32_t *key, uint32_t r3) {
    const uint32_t mode = lookup_fast_mode;
    STAT(LOOKUP_STAT_CALLS);
    if (mode == 0) {
        g_original(out, reg, key, r3);
        return;
    }
    if (mode == 1) {
        if (lookup_fast_try(out, reg, key) != FAST_DONE) {
            STAT(LOOKUP_STAT_FALLBACK);
            g_original(out, reg, key, r3);
        }
        return;
    }
    const uint32_t k0 = key[0], k1 = key[1];
    uint32_t mine[2] = {0xdeadbeefu, 0xdeadbeefu};
    const int done = lookup_fast_try(mine, reg, key);
    g_original(out, reg, key, r3);
    if (done != FAST_DONE) { STAT(LOOKUP_STAT_FALLBACK); return; }
    STAT(LOOKUP_STAT_VERIFIED);
    if (mine[0] != out[0] || mine[1] != out[1]) {
        STAT(LOOKUP_STAT_MISMATCH);
        const uint32_t i = g_mismatch_logged;
        if (i < 8) {
            g_mismatch_log[i][0] = k0;
            g_mismatch_log[i][1] = k1;
            g_mismatch_log[i][2] = mine[0];
            g_mismatch_log[i][3] = mine[1];
            g_mismatch_log[i][4] = out[0];
            g_mismatch_log[i][5] = out[1];
            g_mismatch_log[i][6] = (uint32_t)(uintptr_t)__builtin_return_address(0) - g_text_base - 4;
            g_mismatch_logged = i + 1;
        }
    }
}

unsigned lookup_fast_mismatch(unsigned i, uint32_t v[7]) {
    if (i >= g_mismatch_logged) return 0;
    for (int j = 0; j < 7; ++j) v[j] = g_mismatch_log[i][j];
    return 1;
}
#else
static void lookup_entry(uint32_t *out, const uint8_t *reg, const uint32_t *key, uint32_t r3) {
    if (lookup_fast_try(out, reg, key) != FAST_DONE) g_original(out, reg, key, r3);
}
#endif

/* FNV-1a over every instruction range whose behaviour this file mirrors. */
static const struct { uint32_t start, end; } k_ranges[] = {
    {0xb8e2a0u, 0xb8e480u}, /* the lookup itself */
    {0xb8a210u, 0xb8a33cu}, /* std::map step and slot validation */
    {0xb92c04u, 0xb92ca0u}, /* string pool singleton */
    {0xb92e5cu, 0xb92fe0u}, /* string fetch, refcount ++/-- */
    {0xb93510u, 0xb93520u}, /* direct handle assign */
    {0xb9355cu, 0xb93600u}, /* key decoding, empty result */
    {0xb935fcu, 0xb936f0u}, /* result packing, handle field readers */
    {0xb97964u, 0xb97a28u}, /* 12-byte key compare */
};
#define LOOKUP_FAST_EXPECTED_FNV 0x7039e16bu /* PvZ2 4.5.2 libPVZ2.so */

uint32_t lookup_fast_fingerprint(uintptr_t text_base) {
    uint32_t h = 2166136261u;
    for (unsigned r = 0; r < sizeof(k_ranges) / sizeof(k_ranges[0]); ++r)
        for (uint32_t a = k_ranges[r].start; a < k_ranges[r].end; ++a) {
            h ^= *(const uint8_t *)(text_base + a);
            h *= 16777619u;
        }
    return h;
}

int lookup_fast_prepare(uintptr_t text_base) {
    g_text_base = text_base;
    return lookup_fast_fingerprint(text_base) == LOOKUP_FAST_EXPECTED_FNV ? 0 : -1;
}

void *lookup_fast_entry(void) { return (void *)lookup_entry; }
void lookup_fast_set_original(void *original) { g_original = (lookup_fast_fn)original; }
