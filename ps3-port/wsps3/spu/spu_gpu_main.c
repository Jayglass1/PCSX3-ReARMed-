#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include "spu_job.h"

#define TAG 1
extern void spu_thread_exit(int);

static spu_ctl_t ctl __attribute__((aligned(128)));
static unsigned char buf[2][16384] __attribute__((aligned(128)));

static void wait_tag(unsigned t) { mfc_write_tag_mask(1u << t); mfc_read_tag_status_all(); }

static void job_test(void)
{
    /* sum the source buffer in 16K chunks, then fill our slice of the destination */
    u64w src = ctl.ea_a; u32w n = ctl.n_a, i, sum = 0, x = 0;
    unsigned dec0 = spu_readch(SPU_RdDec);
    for (i = 0; i < n; i += 16384) {
        u32w len = n - i < 16384 ? n - i : 16384, k, base = i / 4;
        mfc_get(buf[0], src + i, len, 2, 0, 0); wait_tag(2);
        const u32w *p = (const u32w *)buf[0];
        for (k = 0; k < len / 4; k++) { sum += p[k]; x ^= p[k] + base + k; }
    }
    ctl.res[0] = sum; ctl.res[1] = x;
    /* write pattern into destination slice */
    {
        u64w dst = ctl.ea_b; u32w m = ctl.n_b, o, k;
        vec_uint4 pat = spu_splats(0xA5000000u | ctl.worker);
        for (k = 0; k < 16384 / 16; k++) ((vec_uint4 *)buf[1])[k] = pat;
        for (o = 0; o < m; o += 16384) {
            u32w len = m - o < 16384 ? m - o : 16384;
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
        if (ctl.cmd == SJ_TEST) job_test();
        ctl.done = sig;
        mfc_put(&ctl, ea, sizeof ctl, TAG, 0, 0); wait_tag(TAG);
    }
    spu_thread_exit(0);
    return 0;
}
