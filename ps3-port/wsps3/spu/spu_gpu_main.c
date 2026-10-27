#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <string.h>
#include "spu_job.h"

#define TAG 1
extern void spu_thread_exit(int);

static spu_ctl_t ctl __attribute__((aligned(128)));
static unsigned char buf[2][4096] __attribute__((aligned(128)));

static void wait_tag(unsigned t) { mfc_write_tag_mask(1u << t); mfc_read_tag_status_all(); }
void spu_dma_get(void *ls, u64w ea, unsigned size) { mfc_get(ls, ea, size, 4, 0, 0); wait_tag(4); }
void spu_dma_put(void *ls, u64w ea, unsigned size) { mfc_put(ls, ea, size, 4, 0, 0); wait_tag(4); }
#define SR_TICK() spu_readch(SPU_RdDec)
static u32w sr_dbgv[4] __attribute__((aligned(16)));
static u64w sr_dbg_ea;
#define SR_DBG(band, idx, kind) do { sr_dbgv[0] = (band); sr_dbgv[1] = (idx); sr_dbgv[2] = (kind); sr_dbgv[3]++; mfc_putf(sr_dbgv, sr_dbg_ea, 16, 6, 0, 0); } while (0)
#include "spu_render.h"

static void job_test(void)
{
    /* sum the source buffer in 16K chunks, then fill our slice of the destination */
    u64w src = ctl.ea_a; u32w n = ctl.n_a, i, sum = 0, x = 0;
    unsigned dec0 = spu_readch(SPU_RdDec);
    for (i = 0; i < n; i += 4096) {
        u32w len = n - i < 4096 ? n - i : 4096, k, base = i / 4;
        mfc_get(buf[0], src + i, len, 2, 0, 0); wait_tag(2);
        const u32w *p = (const u32w *)buf[0];
        for (k = 0; k < len / 4; k++) { sum += p[k]; x ^= p[k] + base + k; }
    }
    ctl.res[0] = sum; ctl.res[1] = x;
    /* write pattern into destination slice */
    {
        u64w dst = ctl.ea_b; u32w m = ctl.n_b, o, k;
        vec_uint4 pat = spu_splats(0xA5000000u | ctl.worker);
        for (k = 0; k < 4096 / 16; k++) ((vec_uint4 *)buf[1])[k] = pat;
        for (o = 0; o < m; o += 4096) {
            u32w len = m - o < 4096 ? m - o : 4096;
            mfc_put(buf[1], dst + o, len, 3, 0, 0); wait_tag(3);
        }
    }
    ctl.res[2] = dec0 - spu_readch(SPU_RdDec); /* ticks (decrementer counts down) */
}

int main(unsigned long long ea, unsigned long long idx, unsigned long long a3, unsigned long long a4)
{
    spu_writech(SPU_WrDec, 0xffffffffu);
    for (;;) {
        unsigned sig = spu_read_signal1();
        mfc_get(&ctl, ea, sizeof ctl, TAG, 0, 0); wait_tag(TAG);
        if (ctl.cmd == SJ_QUIT) break;
        sr_dbg_ea = ea + 80; sr_dbgv[3] = 0;
        if (ctl.cmd == SJ_TEST) job_test();
        else if (ctl.cmd == SJ_RENDER) { unsigned t0 = spu_readch(SPU_RdDec); sr_render(ctl.ea_a, ctl.n_a, ctl.ea_b, ctl.ea_c, ctl.hi, ctl.worker, ctl.n_b); ctl.res[0] = t0 - spu_readch(SPU_RdDec); ctl.res[1] = sr_p[0]; ctl.res[2] = sr_p[1]; ctl.res[3] = sr_p[2]; ctl.res[4] = sr_p[3]; ctl.res[5] = sr_p[4]; ctl.res[6] = sr_p[5]; memcpy(ctl.kt, sr_kt, sizeof ctl.kt); memcpy(ctl.kc, sr_kc, sizeof ctl.kc); memcpy(ctl.ph, sr_ph, sizeof sr_ph); memcpy(ctl.kp, sr_kp, sizeof ctl.kp); memcpy(ctl.ks, sr_ks, sizeof ctl.ks); }
        ctl.done = sig;
        mfc_put(&ctl, ea, sizeof ctl, TAG, 0, 0); wait_tag(TAG);
    }
    spu_thread_exit(0);
    return 0;
}
