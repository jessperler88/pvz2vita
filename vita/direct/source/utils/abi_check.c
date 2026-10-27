/* Diagnostic: detect game calls that return with callee-saved registers
 * (r4-r11) or the stack pointer changed. Built only with -DPVZ2_ABI_CHECK=1.
 * abi_check_call loads known values into r4-r11, calls fn(a0, a1), and
 * stores r4-r11 and the sp difference into out[0..8]. It always restores the
 * caller's own registers, so the main loop is shielded either way. */
#include "abi_check.h"

#if PVZ2_ABI_CHECK
#include "utils/telemetry.h"

__asm__(
    ".text\n"
    ".arm\n"
    ".align 2\n"
    ".global abi_check_call\n"
    ".type abi_check_call, %function\n"
    "abi_check_call:\n"
    "    push {r4-r11, lr}\n"
    "    sub sp, sp, #12\n"
    "    str r3, [sp]\n"           /* out */
    "    str sp, [sp, #4]\n"       /* sp before the call */
    "    mov ip, r0\n"
    "    mov r0, r1\n"
    "    mov r1, r2\n"
    "    ldr r4, =0xabc00004\n"
    "    ldr r5, =0xabc00005\n"
    "    ldr r6, =0xabc00006\n"
    "    ldr r7, =0xabc00007\n"
    "    ldr r8, =0xabc00008\n"
    "    ldr r9, =0xabc00009\n"
    "    ldr r10, =0xabc0000a\n"
    "    ldr r11, =0xabc0000b\n"
    "    blx ip\n"
    "    mov r0, sp\n"
    "    ldr r3, [sp]\n"
    "    stm r3!, {r4-r11}\n"
    "    ldr r1, [sp, #4]\n"
    "    sub r0, r0, r1\n"
    "    str r0, [r3]\n"           /* sp after - sp before */
    "    add sp, sp, #12\n"
    "    pop {r4-r11, pc}\n"
    ".ltorg\n"
    ".thumb\n"
);

void abi_check_report(const char *what, unsigned frame, const uint32_t out[9]) {
    static unsigned reports;
    int bad = out[8] != 0;
    for (unsigned i = 0; i < 8; ++i) bad |= out[i] != 0xabc00004u + i;
    if (!bad || reports >= 24) return;
    ++reports;
    telemetry_log("ABI", "%s frame=%u r4=%08x r5=%08x r6=%08x r7=%08x r8=%08x r9=%08x r10=%08x r11=%08x sp_delta=%d",
                  what, frame, (unsigned)out[0], (unsigned)out[1], (unsigned)out[2], (unsigned)out[3],
                  (unsigned)out[4], (unsigned)out[5], (unsigned)out[6], (unsigned)out[7], (int)out[8]);
}
#endif
