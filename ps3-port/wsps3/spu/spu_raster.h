/* SPU renderer core: PEOPS polygon rasterizer (generated from soft.c) drawing into a band buffer.
 * Portable: compiled for the SPU (spu_gpu_main.c) and for x86 (host_test) with TEXB/TEXW provided by the includer. */
#ifndef SPU_RASTER_H
#define SPU_RASTER_H
#include <stdint.h>
#include <string.h>
#include "spu_job.h"

#ifdef __SPU__
 #define GETLE16(p) (__builtin_bswap16(*(const unsigned short *)(p)))
 #define PUTLE16(p, v) do { *(unsigned short *)(p) = __builtin_bswap16((unsigned short)(v)); } while (0)
 #define SWAP16_C(x) ((((x) >> 8) & 0xff) | (((x) << 8) & 0xff00))
#else
 #define GETLE16(p) (*(const unsigned short *)(p))
 #define PUTLE16(p, v) do { *(unsigned short *)(p) = (unsigned short)(v); } while (0)
 #define SWAP16_C(x) (x)
#endif


typedef uint32_t __attribute__((may_alias)) u32a;
/* 32-bit "two pixel" access built from halfwords so odd (2-byte aligned) addresses work on the SPU */
static inline uint32_t spu_ld32(const void *p) {
#ifdef __SPU__
    if (!((uintptr_t)p & 3)) return __builtin_bswap32(*(const u32a *)p);   /* aligned: one load + one swap */
#endif
    const unsigned short *h = (const unsigned short *)p; return (uint32_t)GETLE16(h) | ((uint32_t)GETLE16(h + 1) << 16); }
static inline void spu_st32(void *p, uint32_t v) {
#ifdef __SPU__
    if (!((uintptr_t)p & 3)) { *(u32a *)p = __builtin_bswap32(v); return; }
#endif
    unsigned short *h = (unsigned short *)p; PUTLE16(h, v & 0xffff); PUTLE16(h + 1, v >> 16); }
#define GETLE32(p) spu_ld32(p)
#define PUTLE32(p, v) spu_st32((p), (v))
#define LOWORD(l)           ((unsigned short)(l))
#define HIWORD(l)           ((unsigned short)(((uint32_t)(l) >> 16) & 0xFFFF))
#define HALFBRIGHTMODE3
#define FASTSOLID
#define X32TCOL1(x)  ((x & 0x001f001f)<<7)
#define X32TCOL2(x)  ((x & 0x03e003e0)<<2)
#define X32TCOL3(x)  ((x & 0x7c007c00)>>3)
#define X32COL1(x)   (x & 0x001f001f)
#define X32COL2(x)   ((x>>5) & 0x001f001f)
#define X32COL3(x)   ((x>>10) & 0x001f001f)
#define X32ACOL1(x)  (x & 0x001e001e)
#define X32ACOL2(x)  ((x>>5) & 0x001e001e)
#define X32ACOL3(x)  ((x>>10) & 0x001e001e)
#define X32BCOL1(x)  (x & 0x001c001c)
#define X32BCOL2(x)  ((x>>5) & 0x001c001c)
#define X32BCOL3(x)  ((x>>10) & 0x001c001c)
#define X32PSXCOL(r,g,b) ((g<<10)|(b<<5)|r)

#define BOOL unsigned short
#define TRUE 1
#define FALSE 0
#define min(a,b) (((a) < (b)) ? (a) : (b))
#define max(a,b) (((a) > (b)) ? (a) : (b))
#define __int64 long long
static int g_il;
#define checkInterlace(i) (g_il ? (((i) & 1) ^ (g_il - 1)) : 0)
static int dshift = 10;   /* log2 of the destination row stride in pixels: 10 native, 11 for the 2x buffer */

#define XCOL1(x)     (x & 0x1f)
#define XCOL2(x)     (x & 0x3e0)
#define XCOL3(x)     (x & 0x7c00)
#define XCOL1D(x)     (x & 0x1f)
#define XCOL2D(x)     ((x>>5) & 0x1f)
#define XCOL3D(x)     ((x>>10) & 0x1f)
#define XPSXCOL(r,g,b) ((r&0x1f)|(b&0x3e0)|(g&0x7c00))

static const unsigned char dithertable[16] = { 7, 0, 6, 1, 2, 5, 3, 4, 1, 6, 0, 7, 4, 3, 5, 2 };

/* state the soft.c routines expect */
static unsigned short *psxDst;          /* band buffer biased so psxDst[(y<<11)+x] addresses row y */
static int32_t drawX, drawY, drawW, drawH;
static short lx0, ly0, lx1, ly1, lx2, ly2, lx3, ly3;
static short Ymin, Ymax;
static int32_t DrawSemiTrans;
static int32_t bCheckMask;
static int32_t sSetMask;
static uint32_t lSetMask;
static int iDither;
static int32_t GlobalTextAddrX, GlobalTextAddrY, GlobalTextABR;
static int32_t g_m1, g_m2, g_m3;
static int32_t g_modtab;   /* 4-bit modulated textured polygon: sr_modv[] holds the final colour of every CLUT entry */
extern uint32_t sr_modv[16];
static int32_t g_plain;   /* no mask test and unmodulated colour (g_m == 128) */
static int32_t GlobalTextTP;
static uint32_t spr_gd2;
static struct { struct { short x, y; } DrawOffset; } PSXDisplay;

extern unsigned short sr_clutv[256];
static void sr_clut_load(unsigned base, unsigned n);
static void sr_mod_build(void);
#define TEXB_SRC(e) TEXB(YAdjust + (e))
/* TEXB(byteOffset) / TEXW(wordIndex) come from the includer */

static int spu_spr_fast(int sx, int sy, int sw, int sh, int tx0, int ty0);
#include "spu_soft_gen.inc"

/* Sprite rows for 4-bit / 8-bit CLUT textures: texels are streamed straight out of the texture cache one
 * 128-byte line at a time (no per-texel cache lookup).
 * mode 0: opaque 4-bit via the per-sprite colour table, 1: GetTextureTransColG_S (opaque, modulated). */
#define SPR_EMIT(IDX) do { unsigned ix_ = (IDX); \
    if (mode == 0) { uint32_t v_ = sr_modv[ix_]; if (v_) PUTLE16(d, v_); } \
    else GetTextureTransColG_S(d, sr_clutv[ix_]); \
    d++; } while (0)
static inline __attribute__((always_inline)) void spr_rows(int sx, int sy, int sw, int sh, int tx0, int ty0, const int nib, const int mode)
{
    uint32_t base = ((uint32_t)GlobalTextAddrX << 1) + ((uint32_t)ty0 << 11);
    int cy;
    for (cy = 0; cy < sh; cy++) {
        unsigned short *d;
        uint32_t off;
        int n = sw, hi = nib ? (tx0 & 1) : 0;
        if (checkInterlace(sy + cy)) continue;
        d = psxDst + ((sy + cy) << 10) + sx;
        off = base + ((uint32_t)cy << 11) + (uint32_t)(nib ? (tx0 >> 1) : tx0);
        while (n > 0) {
            const unsigned char *p = sr_texp(off);
            int avail = 128 - (int)(off & 127), m;
            off += (uint32_t)avail;
            if (nib) {
                int cap = avail * 2 - hi;
                m = n < cap ? n : cap; n -= m;
                if (hi) { SPR_EMIT(p[0] >> 4); p++; m--; hi = 0; }
                while (m >= 2) { unsigned b = *p++; SPR_EMIT(b & 15); SPR_EMIT(b >> 4); m -= 2; }
                if (m) { SPR_EMIT(*p & 15); }
            } else {
                int k;
                m = n < avail ? n : avail; n -= m;
                for (k = 0; k < m; k++) SPR_EMIT(p[k]);
            }
        }
    }
}
/* semi-transparent / mask-tested sprites: same pairing as DrawSoftwareSprite (paired GetTextureTransColG32_SPR) on a row copied out of the cache */
static unsigned char spr_rowbuf[1088] __attribute__((aligned(16)));
static void spr_rows_semi(int sx, int sy, int sw, int sh, int tx0, int ty0, const int nib)
{
    uint32_t base = ((uint32_t)GlobalTextAddrX << 1) + ((uint32_t)ty0 << 11);
    int cy, hi = nib ? (tx0 & 1) : 0, nb = nib ? ((hi + sw + 1) >> 1) : sw;
    for (cy = 0; cy < sh; cy++) {
        unsigned short *d;
        uint32_t off;
        int got = 0, rem, k;
        const unsigned char *p;
        if (checkInterlace(sy + cy)) continue;
        d = psxDst + ((sy + cy) << 10) + sx;
        off = base + ((uint32_t)cy << 11) + (uint32_t)(nib ? (tx0 >> 1) : tx0);
        while (got < nb) {
            const unsigned char *q = sr_texp(off);
            int avail = 128 - (int)(off & 127), m = nb - got < avail ? nb - got : avail;
            memcpy(spr_rowbuf + got, q, m); got += m; off += (uint32_t)avail;
        }
        p = spr_rowbuf;
        if (nib) {
            if (hi) { GetTextureTransColG_SPR(d, sr_clutv[p[0] >> 4]); d++; p++; }
            rem = sw - hi;
            for (k = 0; k < (rem >> 1); k++, d += 2) { unsigned b = *p++; GetTextureTransColG32_SPR((uint32_t *)d, ((uint32_t)sr_clutv[b >> 4] << 16) | sr_clutv[b & 15]); }
            if (rem & 1) GetTextureTransColG_SPR(d, sr_clutv[*p & 15]);
        } else {
            for (k = 0; k < (sw >> 1); k++, d += 2, p += 2) GetTextureTransColG32_SPR((uint32_t *)d, ((uint32_t)sr_clutv[p[1]] << 16) | sr_clutv[p[0]]);
            if (sw & 1) GetTextureTransColG_SPR(d, sr_clutv[*p]);
        }
    }
}
static int spu_spr_fast(int sx, int sy, int sw, int sh, int tx0, int ty0)
{
    if (GlobalTextTP > 1 || sw > 1024) return 0;
    if (GlobalTextTP == 0) {
        if (!bCheckMask && !DrawSemiTrans) { sr_mod_build(); spr_rows(sx, sy, sw, sh, tx0, ty0, 1, 0); }
        else spr_rows_semi(sx, sy, sw, sh, tx0, ty0, 1);
    } else {
        if (!bCheckMask && !DrawSemiTrans) spr_rows(sx, sy, sw, sh, tx0, ty0, 0, 1);
        else spr_rows_semi(sx, sy, sw, sh, tx0, ty0, 0);
    }
    return 1;
}


/* tile: mirrors FillSoftwareAreaTrans() after its clipping (done on the PPU); only rows of this band are drawn */
static void spu_fill_tile(const spu_prim_t *p, int by0, int by1)
{
    int x0 = p->x[0], y0 = p->y[0], x1 = p->x[1], y1 = p->y[1];
    unsigned short col = (unsigned short)p->col[0];
    int dx = x1 - x0, i, j, ya = y0 < by0 ? by0 : y0, yb = y1 > by1 ? by1 : y1;
    if (!bCheckMask && !DrawSemiTrans) {   /* opaque: plain stores, pairs where aligned */
        unsigned short v = (unsigned short)(col | sSetMask);
        uint32_t pv = ((uint32_t)v << 16) | v;
        for (i = ya; i < yb; i++) {
            unsigned short *d; int n = dx;
            if (checkInterlace(i)) continue;
            d = psxDst + (i << dshift) + x0;
            if (((uintptr_t)d & 2) && n) { PUTLE16(d, v); d++; n--; }
            while (n >= 2) { spu_st32(d, pv); d += 2; n -= 2; }
            if (n) PUTLE16(d, v);
        }
    } else if (dx & 1) {
        for (i = ya; i < yb; i++) {
            unsigned short *d;
            if (checkInterlace(i)) continue;
            d = psxDst + (i << dshift) + x0;
            for (j = 0; j < dx; j++) GetShadeTransCol(d++, col);
        }
    } else {
        uint32_t lcol = lSetMask | ((uint32_t)col << 16) | col;
        int dw = dx >> 1;
        for (i = ya; i < yb; i++) {
            unsigned short *d;
            if (checkInterlace(i)) continue;
            d = psxDst + (i << dshift) + x0;
            if (!bCheckMask && !DrawSemiTrans) for (j = 0; j < dx; j++) PUTLE16(d + j, (unsigned short)(col | sSetMask));
            else for (j = 0; j < dw; j++, d += 2) GetShadeTransCol32((uint32_t *)d, lcol);
        }
    }
}

/* draw one polygon record into the current band; mirrors drawPoly{3,4}{F,G,FT,GT}() in soft.c */
static void spu_draw_poly(const spu_prim_t *p, int by0, int by1)
{
    int tp = p->tp;
    /* band rows are by0..by1-1; the buffer has one guard row above and below so a clip of at least two rows never collapses */
    drawX = p->clip[0]; drawY = p->clip[1] > by0 - 1 ? p->clip[1] : by0 - 1; drawW = p->clip[2]; drawH = p->clip[3] < by1 ? p->clip[3] : by1;
    g_il = p->il;
    DrawSemiTrans = p->semi; bCheckMask = p->mask; GlobalTextABR = p->abr; iDither = p->dither;
    sSetMask = p->setmask; lSetMask = ((uint32_t)p->setmask << 16) | p->setmask;
    GlobalTextAddrX = p->tpx; GlobalTextAddrY = p->tpy;
    g_m1 = p->gm[0]; g_m2 = p->gm[1]; g_m3 = p->gm[2];
    g_plain = !bCheckMask && g_m1 == 128 && g_m2 == 128 && g_m3 == 128;
    if (p->pk == 4) {
        if (p->tp < 2) sr_clut_load((((unsigned)p->col[0] >> 22) & 511) * 1024 + (((unsigned)p->col[0] >> 12) & 0x3f0), p->tp == 0 ? 16 : 256);
    } else if (p->pk >= 2 && p->pk != 5 && tp < 2) sr_clut_load((unsigned)p->cly * 1024 + (unsigned)p->clx, tp == 0 ? 16 : 256);
    g_modtab = 0;
    if (p->pk == 2 && tp == 0 && !p->semi && !p->mask) { sr_mod_build(); g_modtab = 1; }
    const short *x = p->x, *y = p->y, *tx = p->tx, *ty = p->ty;
    short clX = p->clx, clY = p->cly;
    const int *c = p->col;
    lx0 = x[0]; ly0 = y[0]; lx1 = x[1]; ly1 = y[1]; lx2 = x[2]; ly2 = y[2]; lx3 = x[3]; ly3 = y[3];

    if (p->pk == 5) { spu_fill_tile(p, by0, by1); return; }
    if (p->pk == 4) {
        GlobalTextTP = tp; spr_gd2 = (uint32_t)c[0];
        PSXDisplay.DrawOffset.x = x[1]; PSXDisplay.DrawOffset.y = y[1];
        lx0 = x[0]; ly0 = y[0];
        DrawSoftwareSprite((short)tx[1], (short)ty[1], tx[0], ty[0]);
        return;
    }
    if (p->nv == 3) {
        switch (p->pk) {
        case 0: drawPoly3Fi(x[0], y[0], x[1], y[1], x[2], y[2], c[0]); break;
        case 1: drawPoly3Gi(x[0], y[0], x[1], y[1], x[2], y[2], c[0], c[1], c[2]); break;
        case 2:
            if (tp == 0)      drawPoly3TEx4(x[0], y[0], x[1], y[1], x[2], y[2], tx[0], ty[0], tx[1], ty[1], tx[2], ty[2], clX, clY);
            else if (tp == 1) drawPoly3TEx8(x[0], y[0], x[1], y[1], x[2], y[2], tx[0], ty[0], tx[1], ty[1], tx[2], ty[2], clX, clY);
            else              drawPoly3TD(x[0], y[0], x[1], y[1], x[2], y[2], tx[0], ty[0], tx[1], ty[1], tx[2], ty[2]);
            break;
        default:
            if (tp == 0)      drawPoly3TGEx4(x[0], y[0], x[1], y[1], x[2], y[2], tx[0], ty[0], tx[1], ty[1], tx[2], ty[2], clX, clY, c[0], c[1], c[2]);
            else if (tp == 1) drawPoly3TGEx8(x[0], y[0], x[1], y[1], x[2], y[2], tx[0], ty[0], tx[1], ty[1], tx[2], ty[2], clX, clY, c[0], c[1], c[2]);
            else              drawPoly3TGD(x[0], y[0], x[1], y[1], x[2], y[2], tx[0], ty[0], tx[1], ty[1], tx[2], ty[2], c[0], c[1], c[2]);
            break;
        }
    } else {
        switch (p->pk) {
        case 0: drawPoly4F(c[0]); break;
        case 1: /* two g-shaded triangles, like drawPoly4G() */
            drawPoly3Gi(lx1, ly1, lx3, ly3, lx2, ly2, c[1], c[3], c[2]);
            drawPoly3Gi(lx0, ly0, lx1, ly1, lx2, ly2, c[0], c[1], c[2]);
            break;
        case 2:
            if (tp == 0)      drawPoly4TEx4(x[0], y[0], x[1], y[1], x[2], y[2], x[3], y[3], tx[0], ty[0], tx[1], ty[1], tx[2], ty[2], tx[3], ty[3], clX, clY);
            else if (tp == 1) drawPoly4TEx8(x[0], y[0], x[1], y[1], x[2], y[2], x[3], y[3], tx[0], ty[0], tx[1], ty[1], tx[2], ty[2], tx[3], ty[3], clX, clY);
            else              drawPoly4TD(x[0], y[0], x[1], y[1], x[2], y[2], x[3], y[3], tx[0], ty[0], tx[1], ty[1], tx[2], ty[2], tx[3], ty[3]);
            break;
        default:
            if (tp == 0)      drawPoly4TGEx4(x[0], y[0], x[1], y[1], x[2], y[2], x[3], y[3], tx[0], ty[0], tx[1], ty[1], tx[2], ty[2], tx[3], ty[3], clX, clY, c[0], c[1], c[2], c[3]);
            else if (tp == 1) drawPoly4TGEx8(x[0], y[0], x[1], y[1], x[2], y[2], x[3], y[3], tx[0], ty[0], tx[1], ty[1], tx[2], ty[2], tx[3], ty[3], clX, clY, c[0], c[1], c[2], c[3]);
            else              drawPoly4TGD(x[0], y[0], x[1], y[1], x[2], y[2], x[3], y[3], tx[0], ty[0], tx[1], ty[1], tx[2], ty[2], tx[3], ty[3], c[0], c[1], c[2], c[3]);
            break;
        }
    }
}
#endif
