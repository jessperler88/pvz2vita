#ifndef PVZ2_ABI_CHECK_H
#define PVZ2_ABI_CHECK_H

#include <stdint.h>

#if PVZ2_ABI_CHECK
void abi_check_call(void *fn, void *a0, void *a1, uint32_t out[9]);
void abi_check_report(const char *what, unsigned frame, const uint32_t out[9]);
#endif

#endif
