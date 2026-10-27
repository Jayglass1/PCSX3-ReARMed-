/* Band renderer shared by the SPU program and the x86 host test.
 * The includer provides:  spu_dma_get(ls, ea, size)  spu_dma_put(ls, ea, size)  (blocking, size <= 16384, multiple of 16)
 * and must include spu_raster.h AFTER defining TEXB/TEXW via the macros below (they use the cache here). */
#ifndef SPU_RENDER_H
#define SPU_RENDER_H
#include <stdint.h>
#include <string.h>
#include "spu_job.h"

#define SR_BANDS      (512 / SPU_BAND_H)
#define SR_BUFROWS    (SPU_BAND_H + 2)
#define SR_CLINES     512                       /* texture cache lines of 128 bytes = 64 KB */
#define SR_CHUNK      64                        /* prims per list fetch */

static unsigned char sr_band[SR_BUFROWS * SPU_VSTRIDE * 2 + 128] __attribute__((aligned(128)));
static unsigned char sr_tc[SR_CLINES * 128] __attribute__((aligned(128)));
static unsigned sr_tag[SR_CLINES];        /* line+1, 0 = empty */
static spu_prim_t sr_list[SR_CHUNK] __attribute__((aligned(128)));
static u64w sr_vram;
#ifndef SR_TICK
 #define SR_TICK() 0u
#endif
static unsigned sr_ph[5], sr_phl;
#define PH_RESET() (sr_phl = SR_TICK())
#define PH(k) do { unsigned n_ = SR_TICK(); sr_ph[k] += sr_phl - n_; sr_phl = n_; } while (0)
static unsigned sr_kt[12], sr_kc[12], sr_kp[12], sr_ks[12];
static unsigned sr_p[8];   /* 0 miss ticks, 1 misses, 2 band dma ticks, 3 draws, 4 draw ticks, 5 list dma ticks */
#ifdef HOST_TEST
int sr_oob;
#endif

extern void spu_dma_get(void *ls, u64w ea, unsigned size);
extern void spu_dma_put(void *ls, u64w ea, unsigned size);

#ifndef SR_DBG
 #define SR_DBG(band, idx, kind)
#endif
#define SR_AI static inline __attribute__((always_inline))
SR_AI unsigned sr_slot(unsigned line) { return ((line >> 4) * 5 + (line & 15)) & (SR_CLINES - 1); }
static void sr_fill(unsigned slot, unsigned line)
{
    unsigned t0 = SR_TICK();
    spu_dma_get(sr_tc + slot * 128, sr_vram + (u64w)line * 128, 128);
    sr_tag[slot] = line + 1;
    sr_p[0] += t0 - SR_TICK(); sr_p[1]++;
}
SR_AI unsigned char sr_texb(uint32_t off)
{
    unsigned line, s;
#ifdef HOST_TEST
    if (off > 0xfffff) sr_oob++;
#endif
    off &= 0xfffff; line = off >> 7; s = sr_slot(line);
    if (sr_tag[s] != line + 1) sr_fill(s, line);
    return sr_tc[(s << 7) | (off & 127)];
}
#ifdef __SPU__
 #define SR_LE16(p) (__builtin_bswap16(*(const unsigned short *)(p)))
#else
 #define SR_LE16(p) (*(const unsigned short *)(p))
#endif
SR_AI unsigned short sr_texw(uint32_t idx)
{
#ifdef HOST_TEST
    if ((idx << 1) > 0xfffff) sr_oob++;
#endif
    uint32_t off = (idx << 1) & 0xfffff;
    unsigned line = off >> 7, s = sr_slot(line);
    if (sr_tag[s] != line + 1) sr_fill(s, line);
    return SR_LE16(sr_tc + ((s << 7) | (off & 127)));
}
#define TEXB(off) sr_texb((uint32_t)(off))
#define TEXW(idx) sr_texw((uint32_t)(idx))

unsigned short sr_clutv[256];
static unsigned sr_cbase = 0xffffffffu, sr_cn;
#include "spu_raster.h"
uint32_t sr_modv[16];
static int sr_mkey[5], sr_mvalid;
/* final colour of each 4-bit CLUT entry for the current modulation (0 = transparent texel, else 0x10000|colour);
 * same arithmetic as the paired path of GetTextureTransColG32 for an opaque, unmasked pixel */
static void sr_mod_build(void)
{
    unsigned k;
    if (sr_mvalid && sr_mkey[0] == (int)sr_cbase && sr_mkey[1] == g_m1 && sr_mkey[2] == g_m2 && sr_mkey[3] == g_m3 && sr_mkey[4] == (int)sSetMask) return;
    for (k = 0; k < 16; k++) {
        uint32_t c = sr_clutv[k], r, g, b, v;
        if (!c) { sr_modv[k] = 0; continue; }
        r = (((c & 0x001f001f) * g_m1) & 0xFF80FF80) >> 7;
        b = ((((c >> 5) & 0x001f001f) * g_m2) & 0xFF80FF80) >> 7;
        g = ((((c >> 10) & 0x001f001f) * g_m3) & 0xFF80FF80) >> 7;
        if (r & 0x7FE0) r = 0x1f; if (b & 0x7FE0) b = 0x1f; if (g & 0x7FE0) g = 0x1f;
        v = (g << 10) | (b << 5) | r | sSetMask | (c & 0x8000);
        sr_modv[k] = 0x10000 | (v & 0xffff);
    }
    sr_mkey[0] = (int)sr_cbase; sr_mkey[1] = g_m1; sr_mkey[2] = g_m2; sr_mkey[3] = g_m3; sr_mkey[4] = (int)sSetMask; sr_mvalid = 1;
}
/* load a CLUT into a local table (all clut reads of the rasterizer index it directly) */
static void sr_clut_load(unsigned base, unsigned n)
{
    unsigned k;
    if (base == sr_cbase && n <= sr_cn) return;
    sr_mvalid = 0;
    for (k = 0; k < n; k++) sr_clutv[k] = sr_texw(base + k);
    sr_cbase = base; sr_cn = n;
}

/* DMA helpers for transfers larger than 16 KB */
static void sr_get_big(void *ls, u64w ea, unsigned size)
{
    while (size) { unsigned n = size > 16384 ? 16384 : size; spu_dma_get(ls, ea, n); ls = (char *)ls + n; ea += n; size -= n; }
}
static void sr_put_big(void *ls, u64w ea, unsigned size)
{
    while (size) { unsigned n = size > 16384 ? 16384 : size; spu_dma_put(ls, ea, n); ls = (char *)ls + n; ea += n; size -= n; }
}

/* draw `n` primitives (list at list_ea) into the bands assigned to `worker` of `nw` */
static void sr_render(u64w list_ea, unsigned n, u64w vram_ea, unsigned worker, unsigned nw)
{
    unsigned band, i, k;
    sr_vram = vram_ea;
    memset(sr_p, 0, sizeof sr_p); memset(sr_kt, 0, sizeof sr_kt); memset(sr_kc, 0, sizeof sr_kc); memset(sr_kp, 0, sizeof sr_kp); memset(sr_ph, 0, sizeof sr_ph); memset(sr_ks, 0, sizeof sr_ks);
    memset(sr_tag, 0, sizeof sr_tag);
    sr_cbase = 0xffffffffu; sr_mvalid = 0;
    for (band = worker; band < SR_BANDS; band += nw) {
        int by0 = band * SPU_BAND_H, by1 = by0 + SPU_BAND_H, loaded = 0;
        psxDst = (unsigned short *)sr_band - ((by0 - 1) << dshift);
        for (i = 0; i < n; i += SR_CHUNK) {
            unsigned m = n - i < SR_CHUNK ? n - i : SR_CHUNK;
            int fetched = 0;
            for (k = 0; k < m; k++) {
                const spu_prim_t *p;
                /* cheap cull needs the record; fetch the chunk lazily only once per band pass */
                if (!fetched) { unsigned t1 = SR_TICK(); sr_get_big(sr_list, list_ea + (u64w)i * sizeof(spu_prim_t), m * sizeof(spu_prim_t)); fetched = 1; sr_p[5] += t1 - SR_TICK(); }
                p = &sr_list[k];
                if (p->y1 < by0 || p->y0 >= by1) continue;
                if (!loaded) {
                    /* rows by0-1 .. by1 (guards included), clamped to VRAM */
                    unsigned t2 = SR_TICK();
                    int r0 = by0 - 1 < 0 ? 0 : by0 - 1, r1 = by1 > 511 ? 511 : by1;
                    sr_get_big(sr_band + (r0 - (by0 - 1)) * SPU_VSTRIDE * 2, vram_ea + (u64w)r0 * SPU_VSTRIDE * 2, (r1 - r0 + 1) * SPU_VSTRIDE * 2);
                    loaded = 1; sr_p[2] += t2 - SR_TICK();
                }
                SR_DBG(band, i + k, p->pk * 16 + p->nv * 4 + p->tp);
                { unsigned t3 = SR_TICK(), dt, kk = p->pk < 2 ? p->pk : p->pk < 4 ? 2 + (p->pk - 2) * 3 + (p->tp > 2 ? 2 : p->tp) : p->pk == 4 ? 8 + (p->tp > 2 ? 2 : p->tp) : 11; spu_draw_poly(p, by0, by1); dt = t3 - SR_TICK(); sr_p[4] += dt; sr_p[3]++; sr_kt[kk] += dt; sr_kc[kk]++;
                  { int xa, xb, ya, yb, q, w, h, top;
                    if (p->pk == 4) { xa = p->x[0] + p->x[1]; xb = xa + p->tx[1]; top = p->y[0] + p->y[1]; yb = top + p->ty[1]; }
                    else if (p->pk == 5) { xa = p->x[0]; xb = p->x[1]; top = p->y[0]; yb = p->y[1]; }
                    else { xa = xb = p->x[0]; top = yb = p->y[0]; for (q = 1; q < p->nv; q++) { if (p->x[q] < xa) xa = p->x[q]; if (p->x[q] > xb) xb = p->x[q]; if (p->y[q] < top) top = p->y[q]; if (p->y[q] > yb) yb = p->y[q]; } }
                    if (xa < p->clip[0]) xa = p->clip[0]; if (xb > p->clip[2]) xb = p->clip[2];
                    ya = top > by0 ? top : by0; if (yb > by1) yb = by1;
                    w = xb - xa; h = yb - ya;
                    if (w > 0 && h > 0) sr_kp[kk] += w * h;
                    if (top < by0) sr_ks[kk] += by0 - top; } }
            }
        }
        if (loaded) { unsigned t4 = SR_TICK();
            sr_put_big(sr_band + SPU_VSTRIDE * 2, vram_ea + (u64w)by0 * SPU_VSTRIDE * 2, SPU_BAND_H * SPU_VSTRIDE * 2); sr_p[2] += t4 - SR_TICK(); }
    }
}
#endif
