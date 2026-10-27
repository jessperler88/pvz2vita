#ifndef PVZ2_LOOKUP_FAST_H
#define PVZ2_LOOKUP_FAST_H

/* Faster replacement for libPVZ2 4.5.2's object lookup (sub_b8e2a0).
 * With -DPVZ2_LOOKUP_AB=1 the test build alternates original / fast / verify
 * every report window and counts results. */

#include <stdint.h>

#define LOOKUP_FAST_OFFSET 0xb8e2a0u
#define LOOKUP_FAST_FIRST  0xe92d4ff0u /* push {r4-r11, lr} */
#define LOOKUP_FAST_SECOND 0xe24dd004u /* sub sp, sp, #4 */

typedef void (*lookup_fast_fn)(uint32_t *out, const uint8_t *registry, const uint32_t *key, uint32_t r3);

/* Returns 0 when the game code matches what the fast path mirrors. */
int lookup_fast_prepare(uintptr_t text_base);
uint32_t lookup_fast_fingerprint(uintptr_t text_base);
void *lookup_fast_entry(void);
void lookup_fast_set_original(void *original);

#if PVZ2_LOOKUP_AB
enum {
    LOOKUP_STAT_DIRECT, LOOKUP_STAT_HIT, LOOKUP_STAT_MISS, LOOKUP_STAT_FALLBACK,
    LOOKUP_STAT_FB_POOL, LOOKUP_STAT_FB_REFCOUNT, LOOKUP_STAT_FB_NAMED, LOOKUP_STAT_FB_SLOT,
    LOOKUP_STAT_VERIFIED, LOOKUP_STAT_MISMATCH, LOOKUP_STAT_CALLS, LOOKUP_STAT_COUNT
};
extern volatile uint32_t lookup_fast_stat[LOOKUP_STAT_COUNT];
extern volatile uint32_t lookup_fast_mode;
/* Copies logged mismatch i: key0, key1, fast0, fast1, orig0, orig1, caller offset. */
unsigned lookup_fast_mismatch(unsigned i, uint32_t v[7]);
#endif

#endif
