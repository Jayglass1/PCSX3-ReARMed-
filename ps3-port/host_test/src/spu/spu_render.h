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

static unsigned char sr_band[SR_BUFROWS * SPU_VSTRIDE * 2] __attribute__((aligned(128)));
static unsigned char sr_tc[SR_CLINES * 128] __attribute__((aligned(128)));
static unsigned short sr_tag[SR_CLINES];        /* line+1, 0 = empty */
static spu_prim_t sr_list[SR_CHUNK] __attribute__((aligned(128)));
static u64w sr_vram;
#ifndef SR_TICK
 #define SR_TICK() 0u
#endif
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
    sr_tag[slot] = (unsigned short)(line + 1);
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

#include "spu_raster.h"

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
    memset(sr_p, 0, sizeof sr_p);
    memset(sr_tag, 0, sizeof sr_tag);
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
                { unsigned t3 = SR_TICK(); spu_draw_poly(p, by0, by1); sr_p[4] += t3 - SR_TICK(); sr_p[3]++; }
            }
        }
        if (loaded) { unsigned t4 = SR_TICK();
            sr_put_big(sr_band + SPU_VSTRIDE * 2, vram_ea + (u64w)by0 * SPU_VSTRIDE * 2, SPU_BAND_H * SPU_VSTRIDE * 2); sr_p[2] += t4 - SR_TICK(); }
    }
}
#endif
