/* Faster replacement for libPVZ2's per-sprite atlas lookup (sub_daa598, 4.5.2).
 *
 * For every sprite the game maps a tile position (x, y) to an atlas page,
 * clamps the sprite rectangle to that page and produces four texture
 * coordinates. The original calls the integer division helpers four times
 * (x / w, y / h, x % w, y % h) through the import table. This version computes
 * each quotient/remainder pair once, inline, with a cached reciprocal. Float
 * math is the same sequence of int->float conversions and divisions, so the
 * results are bit-identical.
 *
 * Arguments (AAPCS, 12 words): out[2], atlas, alt_size, x, y, in/out width,
 * in/out height, then u0, v0, u1, v1 and texture-info[2] outputs. A zero
 * divisor falls back to the original (its division-by-zero behaviour). */
#include "reimpl/sprite_uv_fast.h"
#include "reimpl/fastdiv.h"

#include <stdint.h>

static uintptr_t g_text_base;
static sprite_uv_fn g_original;

/* ---- unsigned quotient/remainder with a small reciprocal cache ---- */
typedef struct { volatile uint32_t d, magic, shift; } DivEntry;
static DivEntry g_div[8];

static inline void udivmod(uint32_t n, uint32_t d, uint32_t *q, uint32_t *r) {
    /* d >= 1 */
    if (!(d & (d - 1))) {
        const unsigned s = (unsigned)__builtin_ctz(d);
        *q = n >> s;
        *r = n & (d - 1);
        return;
    }
    DivEntry *e = &g_div[(d * 2654435761u) >> 29];
    uint32_t magic, l;
    if (e->d == d) {
        magic = e->magic;
        l = e->shift;
    } else {
        l = 32u - (unsigned)__builtin_clz(d - 1); /* d >= 3 */
        magic = (uint32_t)((((1ull << l) - d) << 32) / d) + 1u;
        e->d = 0;
        e->magic = magic;
        e->shift = l;
        e->d = d;
    }
    const uint32_t t = (uint32_t)(((uint64_t)magic * n) >> 32);
    uint32_t qq = (t + ((n - t) >> 1)) >> (l - 1);
    const uint64_t p = (uint64_t)qq * d;
    uint32_t rr = n - (uint32_t)p;
    if (p > n || rr >= d) { /* torn cache entry from another thread */
        qq = n / d;
        rr = n - qq * d;
    }
    *q = qq;
    *r = rr;
}

/* Signed truncating division, matching __aeabi_idiv / __modsi3. d != 0. */
static inline void idivmod(int32_t n, int32_t d, int32_t *q, int32_t *r) {
    if (n >= 0 && d > 0) {
        uint32_t uq, ur;
        udivmod((uint32_t)n, (uint32_t)d, &uq, &ur);
        *q = (int32_t)uq;
        *r = (int32_t)ur;
        return;
    }
    *q = pvz2_aeabi_idiv(n, d);
    *r = pvz2_modsi3(n, d);
}

#define I32(p, off) (*(const int32_t *)((const uint8_t *)(p) + (off)))
#define U32(p, off) (*(const uint32_t *)((const uint8_t *)(p) + (off)))

typedef struct {
    uint32_t out[2];
    int32_t width, height;
    float u0, v0, u1, v1;
    uint32_t info[2];
} UvResult;

enum { UV_FALLBACK = -1, UV_EARLY = 0, UV_FULL = 1 };

static int uv_compute(const uint8_t *atlas, const int32_t *alt, int32_t x, int32_t y,
                      int32_t width_in, int32_t height_in, UvResult *res) {
    if (atlas[0x58] & 0x60) {
        res->out[0] = 0;
        res->out[1] = 0xde1; /* GL_TEXTURE_2D */
        return UV_EARLY;
    }
    const int32_t cell_w = I32(atlas, 0x34), cell_h = I32(atlas, 0x38);
    if (!cell_w || !cell_h) return UV_FALLBACK;
    int32_t qx, rx, qy, ry;
    idivmod(x, cell_w, &qx, &rx);
    idivmod(y, cell_h, &qy, &ry);
    uint32_t page = 0;
    if (x != 0 && y != 0) page = U32(atlas, 0x2c) * (uint32_t)qy + (uint32_t)qx;
    const uint8_t *entry = (const uint8_t *)(U32(atlas, 4) + page * 44u);
    const int32_t page_w = I32(entry, 0x24), page_h = I32(entry, 0x28);

    int32_t right = (int32_t)((uint32_t)width_in + (uint32_t)rx);
    if (right > page_w) right = page_w;
    int32_t bottom = (int32_t)((uint32_t)height_in + (uint32_t)ry);
    if (bottom > page_h) bottom = page_h;
    res->width = (int32_t)((uint32_t)right - (uint32_t)rx);
    res->height = (int32_t)((uint32_t)bottom - (uint32_t)ry);

    float dw, dh;
    if (atlas[0x59] & 2) {
        dw = (float)alt[12];
        dh = (float)alt[13];
    } else {
        dw = (float)page_w;
        dh = (float)page_h;
    }
    res->u0 = (float)rx / dw;
    res->v0 = (float)ry / dh;
    res->u1 = (float)right / dw;
    res->v1 = (float)bottom / dh;
    res->info[0] = U32(entry, 8);
    res->info[1] = U32(entry, 12);
    res->out[0] = U32(entry, 0);
    res->out[1] = U32(entry, 4);
    return UV_FULL;
}

/* Same store order as the original. */
static inline void uv_apply(const UvResult *res, int kind, uint32_t *out, int32_t *width, int32_t *height,
                            float *u0, float *v0, float *u1, float *v1, uint32_t *info) {
    if (kind == UV_FULL) {
        *width = res->width;
        *height = res->height;
        *u0 = res->u0;
        *v0 = res->v0;
        *u1 = res->u1;
        *v1 = res->v1;
        info[0] = res->info[0];
        info[1] = res->info[1];
    }
    out[0] = res->out[0];
    out[1] = res->out[1];
}

#if PVZ2_UV_AB
volatile uint32_t sprite_uv_mode; /* 0 original, 1 fast, 2 verify */
volatile uint32_t sprite_uv_stat[UV_STAT_COUNT];
static volatile uint32_t g_mismatch[8][6];
static volatile uint32_t g_mismatches_logged;
#define STAT(i) (sprite_uv_stat[(i)]++)

static int same_bits(float a, float b) {
    union { float f; uint32_t u; } x = {a}, y = {b};
    return x.u == y.u;
}

static void uv_entry(uint32_t *out, const uint8_t *atlas, const int32_t *alt, int32_t x, int32_t y,
                     int32_t *width, int32_t *height, float *u0, float *v0, float *u1, float *v1,
                     uint32_t *info) {
    const uint32_t mode = sprite_uv_mode;
    STAT(UV_STAT_CALLS);
    if (mode == 0) {
        g_original(out, atlas, alt, x, y, width, height, u0, v0, u1, v1, info);
        return;
    }
    UvResult res;
    const int kind = uv_compute(atlas, alt, x, y, *width, *height, &res);
    if (mode == 1) {
        if (kind == UV_FALLBACK) {
            STAT(UV_STAT_FALLBACK);
            g_original(out, atlas, alt, x, y, width, height, u0, v0, u1, v1, info);
        } else {
            uv_apply(&res, kind, out, width, height, u0, v0, u1, v1, info);
        }
        return;
    }
    g_original(out, atlas, alt, x, y, width, height, u0, v0, u1, v1, info);
    if (kind == UV_FALLBACK) { STAT(UV_STAT_FALLBACK); return; }
    STAT(UV_STAT_VERIFIED);
    int ok = res.out[0] == out[0] && res.out[1] == out[1];
    if (kind == UV_FULL)
        ok = ok && res.width == *width && res.height == *height && same_bits(res.u0, *u0) &&
             same_bits(res.v0, *v0) && same_bits(res.u1, *u1) && same_bits(res.v1, *v1) &&
             res.info[0] == info[0] && res.info[1] == info[1];
    if (!ok) {
        STAT(UV_STAT_MISMATCH);
        const uint32_t i = g_mismatches_logged;
        if (i < 8) {
            g_mismatch[i][0] = (uint32_t)x;
            g_mismatch[i][1] = (uint32_t)y;
            g_mismatch[i][2] = (uint32_t)kind;
            g_mismatch[i][3] = res.out[0] ^ out[0];
            g_mismatch[i][4] = (uint32_t)(res.width - *width) | ((uint32_t)(res.height - *height) << 16);
            union { float f; uint32_t u; } a = {res.u1}, b = {*u1};
            g_mismatch[i][5] = a.u ^ b.u;
            g_mismatches_logged = i + 1;
        }
    }
}

unsigned sprite_uv_mismatch(unsigned i, uint32_t v[6]) {
    if (i >= g_mismatches_logged) return 0;
    for (int j = 0; j < 6; ++j) v[j] = g_mismatch[i][j];
    return 1;
}
#else
static void uv_entry(uint32_t *out, const uint8_t *atlas, const int32_t *alt, int32_t x, int32_t y,
                     int32_t *width, int32_t *height, float *u0, float *v0, float *u1, float *v1,
                     uint32_t *info) {
    UvResult res;
    const int kind = uv_compute(atlas, alt, x, y, *width, *height, &res);
    if (kind == UV_FALLBACK)
        g_original(out, atlas, alt, x, y, width, height, u0, v0, u1, v1, info);
    else
        uv_apply(&res, kind, out, width, height, u0, v0, u1, v1, info);
}
#endif

#define SPRITE_UV_EXPECTED_FNV 0x6ef06d00u /* PvZ2 4.5.2 libPVZ2.so */

uint32_t sprite_uv_fingerprint(uintptr_t text_base) {
    uint32_t h = 2166136261u;
    for (uint32_t a = SPRITE_UV_OFFSET; a < SPRITE_UV_END; ++a) {
        h ^= *(const uint8_t *)(text_base + a);
        h *= 16777619u;
    }
    return h;
}

int sprite_uv_prepare(uintptr_t text_base) {
    g_text_base = text_base;
    return sprite_uv_fingerprint(text_base) == SPRITE_UV_EXPECTED_FNV ? 0 : -1;
}

void *sprite_uv_entry(void) { return (void *)uv_entry; }
void sprite_uv_set_original(void *original) { g_original = (sprite_uv_fn)original; }
