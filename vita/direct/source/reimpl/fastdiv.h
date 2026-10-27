#ifndef PVZ2_FASTDIV_H
#define PVZ2_FASTDIV_H

#include <stdint.h>

/* Cached-reciprocal replacements for the game's 32-bit division helpers.
 * pvz2_fastdiv_install runs after so_relocate; the game's own helpers remain
 * the fallback (division by zero keeps its original behaviour). */
uint32_t pvz2_umodsi3(uint32_t n, uint32_t d);
int32_t pvz2_modsi3(int32_t n, int32_t d);
uint32_t pvz2_aeabi_uidiv(uint32_t n, uint32_t d);
int32_t pvz2_aeabi_idiv(int32_t n, int32_t d);
void pvz2_fastdiv_set_fallbacks(uint32_t (*umod)(uint32_t, uint32_t), int32_t (*mod)(int32_t, int32_t),
                                uint32_t (*udiv)(uint32_t, uint32_t), int32_t (*idiv)(int32_t, int32_t));
#ifndef PVZ2_FASTDIV_HOST_TEST
#include <so_util/so_util.h>
int pvz2_fastdiv_install(so_module *mod);
#endif

#if PVZ2_FASTDIV_STATS
extern volatile uint32_t pvz2_fastdiv_calls, pvz2_fastdiv_misses;
#endif

#endif
