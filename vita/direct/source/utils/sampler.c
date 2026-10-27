/* Statistical CPU sampler for libPVZ2 code (measurement builds only).
 *
 * Built with -DPVZ2_SAMPLER=1 and active only when ux0:data/pvz2/profile/
 * exists. A helper thread periodically removes EXEC permission from the game
 * library's code. The next thread to run game code takes a prefetch abort; the
 * handler records its PC, LR and (main thread only) the top of its stack, then
 * restores EXEC and resumes. Samples go to userdata/profile.bin.
 *
 * kubridge remaps the pages while changing permission, so a thread reading
 * literal data from the code region at that moment takes a data abort. Those
 * aborts are retried. Every other fault is passed to the previous handler, so
 * genuine crashes still produce a core dump. */
#include "sampler.h"

#if PVZ2_SAMPLER
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>
#include "telemetry.h"

/* Declared here: the project's bundled kubridge header predates these calls. */
typedef struct {
    uint32_t r[13], sp, lr, pc;
    uint64_t vfp[32];
    uint32_t spsr, fpscr, fpexc, fsr, far, type;
} SamplerExceptionContext;
typedef void (*SamplerExceptionHandler)(SamplerExceptionContext *);
typedef struct { uint32_t size; } SamplerExceptionHandlerOpt;
int kuKernelRegisterExceptionHandler(uint32_t type, SamplerExceptionHandler handler,
                                     SamplerExceptionHandler *old, SamplerExceptionHandlerOpt *opt);
int kuKernelMemProtect(void *addr, uint32_t len, uint32_t prot);
void kuKernelReleaseExceptionHandler(uint32_t type);
#define PROT_R 0x40
#define PROT_X 0x10
#define EXC_DATA_ABORT 0
#define EXC_PREFETCH_ABORT 1

#define STACK_WORDS 48
#define RING 4096
#define FLUSH_AT 1024
#define PERIOD_US 2000
#define MAGIC 0x31534d50u /* "PMS1" */

typedef struct {
    uint32_t time_us, frame, tid, pc, lr, latency_us, nstack;
    uint32_t stack[STACK_WORDS];
} Sample;

static Sample g_ring[RING];
static atomic_uint g_head, g_tail;
static atomic_uint g_armed;
static atomic_uint g_protected; /* EXEC removed or being restored */
static atomic_uint g_dropped, g_retries;
static uint64_t g_armed_at;
static uintptr_t g_text, g_text_end;
static uint32_t g_text_len;
static SceUID g_main_tid;
static uintptr_t g_main_stack_lo, g_main_stack_hi;
static atomic_uint g_stopped;

/* A fault the sampler does not own: stop sampling, uninstall, and return so the
 * instruction faults again under kubridge's default handler (normal core dump).
 * kubridge reports no previous handler (NULL) when none was installed. */
static void pass_on(uint32_t type) {
    atomic_store_explicit(&g_stopped, 1, memory_order_release);
    atomic_store_explicit(&g_armed, 0, memory_order_release);
    kuKernelReleaseExceptionHandler(EXC_PREFETCH_ABORT);
    kuKernelReleaseExceptionHandler(EXC_DATA_ABORT);
    (void)type;
}
static SamplerExceptionHandler g_old_prefetch, g_old_data;
volatile unsigned pvz2_sampler_frame;

static inline int in_text(uint32_t a) { return a >= g_text && a < g_text_end; }

__attribute__((optimize("no-tree-vectorize")))
static void prefetch_handler(SamplerExceptionContext *ctx) {
    if (!in_text(ctx->pc)) { pass_on(EXC_PREFETCH_ABORT); return; }
    if (atomic_exchange_explicit(&g_armed, 0, memory_order_acq_rel)) {
        const uint64_t now = sceKernelGetSystemTimeWide();
        kuKernelMemProtect((void *)g_text, g_text_len, PROT_R | PROT_X);
        atomic_store_explicit(&g_protected, 0, memory_order_release);
        const unsigned head = atomic_load_explicit(&g_head, memory_order_relaxed);
        if (head - atomic_load_explicit(&g_tail, memory_order_acquire) >= RING) {
            atomic_fetch_add_explicit(&g_dropped, 1, memory_order_relaxed);
            return;
        }
        Sample *s = &g_ring[head % RING];
        const SceUID tid = sceKernelGetThreadId();
        s->time_us = (uint32_t)now;
        s->frame = pvz2_sampler_frame;
        s->tid = (uint32_t)tid;
        s->pc = ctx->pc - g_text;
        s->lr = ctx->lr;
        s->latency_us = (uint32_t)(now - g_armed_at);
        s->nstack = 0;
        if (tid == g_main_tid) {
            uintptr_t sp = ctx->sp & ~3u;
            unsigned n = 0;
            while (n < STACK_WORDS && sp + 4 * n + 4 <= g_main_stack_hi && sp >= g_main_stack_lo) {
                s->stack[n] = ((const volatile uint32_t *)sp)[n];
                ++n;
            }
            s->nstack = n;
        }
        atomic_store_explicit(&g_head, head + 1, memory_order_release);
        return;
    }
    /* The game's code is always executable except while the sampler has EXEC
     * off, so any fetch fault here is ours, even if another thread's restore
     * already cleared g_protected. Make sure EXEC is back on, then retry. */
    if (!atomic_load_explicit(&g_protected, memory_order_acquire))
        kuKernelMemProtect((void *)g_text, g_text_len, PROT_R | PROT_X);
    atomic_fetch_add_explicit(&g_retries, 1, memory_order_relaxed);
}

static void data_handler(SamplerExceptionContext *ctx) {
    /* Reads from the code region only fault while kubridge remaps it. */
    /* The code region is always readable except while kubridge remaps it.
     * Writes into it (FSR WnR bit) are genuine bugs and keep their crash. */
    if (in_text(ctx->far) && !(ctx->fsr & (1u << 11))) {
        atomic_fetch_add_explicit(&g_retries, 1, memory_order_relaxed);
        return;
    }
    pass_on(EXC_DATA_ABORT);
}

static void flush(FILE *fd) {
    unsigned tail = atomic_load_explicit(&g_tail, memory_order_relaxed);
    const unsigned head = atomic_load_explicit(&g_head, memory_order_acquire);
    while (tail != head) {
        unsigned idx = tail % RING, count = head - tail;
        if (count > RING - idx) count = RING - idx;
        fwrite(&g_ring[idx], sizeof(Sample), count, fd);
        tail += count;
        atomic_store_explicit(&g_tail, tail, memory_order_release);
    }
}

static FILE *g_fd;

static int sampler_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    FILE *const fd = g_fd;
    telemetry_log("SAMPLER", "thread running");
    const uint32_t header[8] = {MAGIC, sizeof(Sample), STACK_WORDS, (uint32_t)g_text,
                                g_text_len, (uint32_t)g_main_tid, PERIOD_US, 0};
    fwrite(header, sizeof(header), 1, fd);
    fflush(fd);
    unsigned idle = 0, loops = 0;
    for (;;) {
        sceKernelDelayThread(PERIOD_US);
        if (atomic_load_explicit(&g_stopped, memory_order_acquire)) {
            kuKernelMemProtect((void *)g_text, g_text_len, PROT_R | PROT_X);
            flush(fd);
            fclose(fd);
            return 0;
        }
        /* Nobody ran game code since the last arm: restore and count it as idle. */
        if (atomic_exchange_explicit(&g_armed, 0, memory_order_acq_rel)) {
            kuKernelMemProtect((void *)g_text, g_text_len, PROT_R | PROT_X);
            atomic_store_explicit(&g_protected, 0, memory_order_release);
            ++idle;
        }
        if (atomic_load_explicit(&g_head, memory_order_relaxed) -
            atomic_load_explicit(&g_tail, memory_order_relaxed) >= FLUSH_AT) { flush(fd); fflush(fd); }
        if ((++loops % 5000u) == 0)
            telemetry_log("SAMPLER", "samples=%u idle=%u dropped=%u retries=%u",
                          atomic_load(&g_head), idle, atomic_load(&g_dropped), atomic_load(&g_retries));
        g_armed_at = sceKernelGetSystemTimeWide();
        atomic_store_explicit(&g_protected, 1, memory_order_release);
        atomic_store_explicit(&g_armed, 1, memory_order_release);
        if (kuKernelMemProtect((void *)g_text, g_text_len, PROT_R) < 0) {
            atomic_store(&g_armed, 0);
            atomic_store(&g_protected, 0);
            telemetry_log("SAMPLER", "kuKernelMemProtect failed; sampler stopped");
            flush(fd);
            fclose(fd);
            return 0;
        }
    }
}

void pvz2_sampler_start(uintptr_t text_base, uint32_t text_len) {
    SceIoStat st;
    if (sceIoGetstat(GAME_DATA_PATH "profile", &st) < 0 || !SCE_S_ISDIR(st.st_mode)) return;
    g_text = text_base;
    g_text_len = (text_len + 0xfffu) & ~0xfffu;
    g_text_end = text_base + g_text_len;
    g_main_tid = sceKernelGetThreadId();
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(g_main_tid, &info) >= 0) {
        g_main_stack_lo = (uintptr_t)info.stack;
        g_main_stack_hi = (uintptr_t)info.stack + info.stackSize;
    }
    SamplerExceptionHandlerOpt opt = {sizeof(opt)};
    int rc1 = kuKernelRegisterExceptionHandler(EXC_PREFETCH_ABORT, prefetch_handler, &g_old_prefetch, &opt);
    int rc2 = kuKernelRegisterExceptionHandler(EXC_DATA_ABORT, data_handler, &g_old_data, &opt);
    telemetry_log("SAMPLER", "handlers prefetch=0x%x data=0x%x text=0x%x len=0x%x main_tid=0x%x stack=0x%x-0x%x",
                  (unsigned)rc1, (unsigned)rc2, (unsigned)g_text, (unsigned)g_text_len,
                  (unsigned)g_main_tid, (unsigned)g_main_stack_lo, (unsigned)g_main_stack_hi);
    if (rc1 < 0 || rc2 < 0) return;
    /* sceIoOpen with CREAT|TRUNC returns EINVAL here; telemetry uses fopen too. */
    g_fd = fopen(DATA_PATH "profile.bin", "wb");
    telemetry_log("SAMPLER", "profile.bin %s", g_fd ? "open" : "FAILED");
    if (!g_fd) return;
    /* Highest user priority so samples are taken on time; it sleeps between arms. */
    SceUID thid = sceKernelCreateThread("pvz2_sampler", sampler_thread, 0x40, 0x4000, 0, 0x70000, NULL);
    int started = thid >= 0 ? sceKernelStartThread(thid, 0, NULL) : -1;
    telemetry_log("SAMPLER", "thread create=0x%x start=0x%x", (unsigned)thid, (unsigned)started);
}
#endif
