#ifndef PVZ2_SPRITE_UV_FAST_H
#define PVZ2_SPRITE_UV_FAST_H

/* Faster replacement for libPVZ2 4.5.2's per-sprite atlas lookup (sub_daa598).
 * With -DPVZ2_UV_AB=1 the test build alternates original / fast / verify. */

#include <stdint.h>

#define SPRITE_UV_OFFSET 0xdaa598u
#define SPRITE_UV_END    0xdaa778u
#define SPRITE_UV_FIRST  0xe92d4ff0u /* push {r4-r11, lr} */
#define SPRITE_UV_SECOND 0xe24dd034u /* sub sp, sp, #52 */

typedef void (*sprite_uv_fn)(uint32_t *out, const uint8_t *atlas, const int32_t *alt, int32_t x, int32_t y,
                             int32_t *width, int32_t *height, float *u0, float *v0, float *u1, float *v1,
                             uint32_t *info);

int sprite_uv_prepare(uintptr_t text_base);
uint32_t sprite_uv_fingerprint(uintptr_t text_base);
void *sprite_uv_entry(void);
void sprite_uv_set_original(void *original);

#if PVZ2_UV_AB
enum { UV_STAT_CALLS, UV_STAT_FALLBACK, UV_STAT_VERIFIED, UV_STAT_MISMATCH, UV_STAT_COUNT };
extern volatile uint32_t sprite_uv_mode;
extern volatile uint32_t sprite_uv_stat[UV_STAT_COUNT];
/* Mismatch i: x, y, kind, out0 xor, width/height delta, u1 bit xor. */
unsigned sprite_uv_mismatch(unsigned i, uint32_t v[6]);
#endif

#endif
