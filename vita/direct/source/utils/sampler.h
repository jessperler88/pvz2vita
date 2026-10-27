#ifndef PVZ2_SAMPLER_H
#define PVZ2_SAMPLER_H

#include <stdint.h>

#if PVZ2_SAMPLER
/* Starts the opt-in CPU sampler (needs ux0:data/pvz2/profile/). Call from the
 * main thread after the game library is loaded. */
void pvz2_sampler_start(uintptr_t text_base, uint32_t text_len);
extern volatile unsigned pvz2_sampler_frame;
#endif

#endif
