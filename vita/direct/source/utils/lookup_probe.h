#ifndef PVZ2_LOOKUP_PROBE_H
#define PVZ2_LOOKUP_PROBE_H

/* Measurement-only timing of libPVZ2's object lookup (sub_b8e2a0).
 * Built only with -DPVZ2_LOOKUP_PROBE=1. */

#include <stddef.h>
#include <stdint.h>

#define LOOKUP_PROBE_OFFSET 0xb8e2a0u
#define LOOKUP_PROBE_FIRST  0xe92d4ff0u /* push {r4-r11, lr} */
#define LOOKUP_PROBE_SECOND 0xe24dd004u /* sub sp, sp, #4 */

#if PVZ2_LOOKUP_PROBE
void *lookup_probe_wrapper(void);
void lookup_probe_set_original(void *original, uintptr_t text_base, uintptr_t text_size);
void lookup_probe_format(char *out, size_t capacity, unsigned frames, unsigned game_us);
#endif

#endif
