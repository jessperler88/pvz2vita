#include "telemetry.h"
#include <so_util/so_util.h>

#include <kubridge.h>

/* 4.5.2 "use_action" animation-event handler (libPVZ2+0x644260 onward).
 * It resolves a weak object reference and passes the result straight to
 * libPVZ2+0x2390d8, which reads target->[0x5c] with no NULL check. If the
 * target was destroyed between the animation starting and its use_action
 * keyframe (seen with Snapdragon Plant Food in a busy Ancient Egypt wave),
 * the weak lookup returns NULL and the game takes a data abort at +0x239130.
 *
 * 0x2390d8 has exactly one caller. Replace its two-instruction call site
 *     mov r0, r4        ->  movs r0, r4
 *     bl  0x2390d8      ->  blne 0x2390d8
 * so a dead target skips the action. The caller ignores the return value
 * and returns 1 either way, so its own control flow is unchanged. */
#define USE_ACTION_SITE 0x64432cu

int use_action452_install(so_module *module) {
    const uintptr_t base = module->text_base;
    uint32_t *site = (uint32_t *)(base + USE_ACTION_SITE);
    /* Fingerprint: the weak-pointer resolve, release and the guarded call. */
    if (*(uint32_t *)(base + 0x64431c) != 0xeb15168eu ||
        *(uint32_t *)(base + 0x644328) != 0xeb153cd3u ||
        *(uint32_t *)(base + 0x644334) != 0xe3a00001u ||
        *(uint32_t *)(base + 0x2390d8) != 0xe92d47f0u) {
        telemetry_log("PATCH", "4.5.2 use_action NULL guard skipped (fingerprint mismatch)");
        return -1;
    }
    if (site[0] == 0xe1b00004u && site[1] == 0x1befd368u) return 0;
    if (site[0] != 0xe1a00004u || site[1] != 0xebefd368u) {
        telemetry_log("PATCH", "4.5.2 use_action NULL guard skipped (call site mismatch)");
        return -1;
    }
    static const uint32_t patched[2] = {0xe1b00004u, 0x1befd368u};
    kuKernelCpuUnrestrictedMemcpy(site, patched, sizeof(patched));
    telemetry_log("PATCH", "4.5.2 use_action NULL target guard installed");
    return 0;
}
