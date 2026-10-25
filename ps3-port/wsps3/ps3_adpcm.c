/* C replacements for the Wii paired-single ADPCM decoders (xaDecode.S) */
#include "psxcommon.h"
static inline s16 sat16(float v){ if (v > 32767.f) return 32767; if (v < -32768.f) return -32768; return (s16)v; }
static void decode(typPcmDecode *t, s16 *dest, int stride_elems, int pcm)
{
    const s16 *bp = (const s16 *)t->vblockp;
    float *st = (float *)t->decpAddr;
    float fy0 = st[0], fy1 = st[1], k0 = t->IK0, k1 = t->IK1;
    int range = t->range, i;
    for (i = 0; i < 7; i++) {
        u32 y = (u16)bp[i];
        s16 v[4];
        if (pcm) { v[0]=(s16)((y<<4)&0xf000); v[1]=(s16)(y&0xf000); v[2]=(s16)((y<<12)&0xf000); v[3]=(s16)((y<<8)&0xf000); }
        else     { v[0]=(s16)((y<<12)&0xf000); v[1]=(s16)((y<<8)&0xf000); v[2]=(s16)((y<<4)&0xf000); v[3]=(s16)(y&0xf000); }
        for (int j = 0; j < 4; j++) {
            float x = (float)(s16)(v[j] >> range);
            float n = x + k0 * fy0 + k1 * fy1;
            fy1 = fy0; fy0 = n;
            *dest = sat16(n); dest += stride_elems;
        }
    }
    st[0] = fy0; st[1] = fy1;
}
void psDecodeBlock16(void *t, s16 *destp, s32 inc_bytes) { decode((typPcmDecode*)t, destp, inc_bytes >> 1, 0); }
void psDecodePcmBlock(void *t, s16 *destp, s32 inc_bytes) { decode((typPcmDecode*)t, destp + 1, inc_bytes >> 1, 1); }
