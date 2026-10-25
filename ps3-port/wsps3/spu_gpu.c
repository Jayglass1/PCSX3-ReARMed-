/* PPU side of the SPU helpers: worker threads, job dispatch, self test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <sys/spu.h>
#include "spu/spu_job.h"

extern const unsigned char ws_spu_gpu_elf[];
extern void SysPrintf(const char *fmt, ...);

#define WS_MAXSPU 6
static spu_ctl_t *sc;              /* control blocks */
static sys_spu_thread_t sthr[WS_MAXSPU];
static sys_spu_group_t sgrp;
static sysSpuImage simg;
static int nspu, spu_seq;
static unsigned long long ea_of(const void *p) { return (unsigned long long)(uintptr_t)p; }
static inline unsigned long long tb(void) { unsigned long long t; __asm__ volatile("mftb %0" : "=r"(t)); return t; }

int ws_spu_count(void) { return nspu; }

int ws_spu_init(int want)
{
    int i, r;
    sysSpuThreadGroupAttribute ga;
    sysSpuThreadAttribute ta;
    sysSpuThreadArgument arg;
    static char gname[] = "wsgpu", tname[] = "wsw";

    if (nspu) return nspu;
    if (want > WS_MAXSPU) want = WS_MAXSPU;
    if (want < 1) return 0;
    r = sysSpuInitialize(6, 0);
    SysPrintf("spu: initialize -> %08x\n", (unsigned)r);
    r = sysSpuImageImport(&simg, ws_spu_gpu_elf, 0);
    if (r) { SysPrintf("spu: image import failed %08x\n", (unsigned)r); return 0; }
    sc = (spu_ctl_t *)memalign(128, sizeof(spu_ctl_t) * WS_MAXSPU);
    if (!sc) return 0;
    memset(sc, 0, sizeof(spu_ctl_t) * WS_MAXSPU);
    ga.nsize = sizeof gname; ga.name = gname; ga.type = 0; ga.option.ct = 0;
    r = sysSpuThreadGroupCreate(&sgrp, want, 100, &ga);
    if (r) { SysPrintf("spu: group create (%d) failed %08x\n", want, (unsigned)r); return 0; }
    for (i = 0; i < want; i++) {
        ta.name = tname; ta.nsize = sizeof tname; ta.option = SPU_THREAD_ATTR_NONE;
        arg.arg0 = ea_of(&sc[i]); arg.arg1 = i; arg.arg2 = 0; arg.arg3 = 0;
        r = sysSpuThreadInitialize(&sthr[i], sgrp, i, &simg, &ta, &arg);
        if (r) { SysPrintf("spu: thread %d init failed %08x\n", i, (unsigned)r); return 0; }
        sysSpuThreadSetConfiguration(sthr[i], SPU_SIGNAL1_OVERWRITE);
    }
    r = sysSpuThreadGroupStart(sgrp);
    if (r) { SysPrintf("spu: group start failed %08x\n", (unsigned)r); return 0; }
    nspu = want;
    SysPrintf("spu: %d workers started\n", nspu);
    return nspu;
}

void ws_spu_shutdown(void)
{
    u32 cause, status; int i;
    if (!nspu) return;
    spu_seq++;
    for (i = 0; i < nspu; i++) { sc[i].cmd = SJ_QUIT; sc[i].seq = spu_seq; }
    __sync_synchronize();
    for (i = 0; i < nspu; i++) sysSpuThreadWriteSignal(sthr[i], 0, spu_seq);
    sysSpuThreadGroupJoin(sgrp, &cause, &status);
    sysSpuImageClose(&simg);
    nspu = 0;
}

/* dispatch to worker i; ws_spu_wait_all collects them */
void ws_spu_kick(int i, unsigned cmd, unsigned long long a, unsigned na, unsigned long long b, unsigned nb)
{
    sc[i].cmd = cmd; sc[i].ea_a = a; sc[i].n_a = na; sc[i].ea_b = b; sc[i].n_b = nb;
    sc[i].worker = i; sc[i].seq = spu_seq; sc[i].done = 0;
    __sync_synchronize();
    sysSpuThreadWriteSignal(sthr[i], 0, spu_seq);
}
int ws_spu_begin(void) { return ++spu_seq; }
/* returns 0 on success, -1 on timeout (ms) */
int ws_spu_wait_all(int timeout_ms)
{
    int i; unsigned long long t0 = tb();
    for (i = 0; i < nspu; i++) {
        while (*(volatile u32 *)&sc[i].done != (u32)spu_seq) {
            if ((tb() - t0) / 79800 > (unsigned long long)timeout_ms) return -1;
        }
    }
    __sync_synchronize();
    return 0;
}
unsigned ws_spu_res(int i, int k) { return sc[i].res[k]; }

/* bring-up check: every worker reads a shared buffer, checksums it and fills its destination slice */
void ws_spu_selftest(void)
{
    const unsigned N = 1 << 20, SL = 64 * 1024;
    unsigned *src = (unsigned *)memalign(128, N), *dst = (unsigned *)memalign(128, SL * WS_MAXSPU);
    unsigned i, sum = 0, x = 0, ok = 1; unsigned long long t0, t1;
    int w, rc;
    if (!nspu || !src || !dst) { SysPrintf("spu: selftest skipped\n"); return; }
    for (i = 0; i < N / 4; i++) { src[i] = i * 2654435761u + 12345; sum += src[i]; x ^= src[i] + i; }
    memset(dst, 0, SL * WS_MAXSPU);
    ws_spu_begin();
    t0 = tb();
    for (w = 0; w < nspu; w++) ws_spu_kick(w, SJ_TEST, ea_of(src), N, ea_of((char *)dst + w * SL), SL);
    rc = ws_spu_wait_all(2000);
    t1 = tb();
    if (rc) { SysPrintf("spu: selftest TIMEOUT\n"); free(src); free(dst); return; }
    for (w = 0; w < nspu; w++) {
        unsigned pat = 0xA5000000u | w, bad = 0;
        for (i = 0; i < SL / 4; i++) if (dst[w * (SL / 4) + i] != pat) bad++;
        if (ws_spu_res(w, 0) != sum || ws_spu_res(w, 1) != x || bad) ok = 0;
        SysPrintf("spu: worker %d sum %s xor %s fill-errors %u spu-ticks %u\n", w, ws_spu_res(w, 0) == sum ? "ok" : "BAD",
                  ws_spu_res(w, 1) == x ? "ok" : "BAD", bad, ws_spu_res(w, 2));
    }
    SysPrintf("spu: selftest %s, %u us for %d workers\n", ok ? "PASSED" : "FAILED", (unsigned)((t1 - t0) * 1000 / 79800), nspu);
    free(src); free(dst);
}

/* ------------------------------------------------------------------ */
/* SPU polygon renderer: the GPU thread records polygon primitives; the SPU workers draw them into VRAM */
#define WS_LISTCAP 6144
static spu_prim_t *plist[2];
static int pcur, pn, pinflight;
static void *spu_vram;
volatile int ws_spu_busy;        /* records pending or a job in flight */
int ws_spu_render_on;            /* set by the menu */
unsigned long long ws_spu_stat[6]; /* 0 jobs, 1 prims, 2 sync waits, 3 wait ticks, 4 max spu ticks, 5 flush-by-full */

void ws_spu_set_vram(void *v) { spu_vram = v; }
int ws_spu_render_ready(void) { return nspu > 0 && plist[0] && spu_vram && ((uintptr_t)spu_vram & 15) == 0; }

static void spu_alloc(void)
{
    if (plist[0]) return;
    plist[0] = (spu_prim_t *)memalign(128, sizeof(spu_prim_t) * WS_LISTCAP);
    plist[1] = (spu_prim_t *)memalign(128, sizeof(spu_prim_t) * WS_LISTCAP);
}
int ws_spu_render_init(void)
{
    if (!nspu) return 0;
    spu_alloc();
    return plist[0] && plist[1];
}

static int spu_poll(void)
{
    int i;
    for (i = 0; i < nspu; i++) if (*(volatile u32 *)&sc[i].done != (u32)spu_seq) return 0;
    return 1;
}
static void spu_wait_inflight(void)
{
    unsigned long long t0;
    int i;
    if (!pinflight) return;
    if (!spu_poll()) {
        t0 = tb(); ws_spu_stat[2]++;
        while (!spu_poll()) {
            if (tb() - t0 > 79800000ULL * 3) { SysPrintf("spu: render job timeout, disabling SPU renderer\n"); ws_spu_render_on = 0; break; }
        }
        ws_spu_stat[3] += tb() - t0;
    }
    __sync_synchronize();
    for (i = 0; i < nspu; i++) if (sc[i].res[0] > ws_spu_stat[4]) ws_spu_stat[4] = sc[i].res[0];
    pinflight = 0;
}
static void spu_submit(void)
{
    int i;
    if (!pn) return;
    spu_wait_inflight();
    ws_spu_begin();
    __sync_synchronize();
    for (i = 0; i < nspu; i++)
        ws_spu_kick(i, SJ_RENDER, ea_of(plist[pcur]), pn, ea_of(spu_vram), nspu);
    ws_spu_stat[0]++; ws_spu_stat[1] += pn;
    pinflight = 1; pcur ^= 1; pn = 0;
}
void ws_spu_cap_add(const spu_prim_t *p)
{
    if (pn >= WS_LISTCAP) { ws_spu_stat[5]++; spu_submit(); }
    plist[pcur][pn++] = *p;
    ws_spu_busy = 1;
}
/* start drawing what has been recorded so far without waiting for it */
void ws_spu_flush(void)
{
    if (pn && (!pinflight || spu_poll())) spu_submit();
}
/* make VRAM current: draw everything recorded and wait */
void ws_spu_sync(void)
{
    if (!ws_spu_busy) return;
    spu_submit();
    spu_wait_inflight();
    ws_spu_busy = 0;
}
