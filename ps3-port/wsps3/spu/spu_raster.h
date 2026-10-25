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


/* 32-bit "two pixel" access built from halfwords so odd (2-byte aligned) addresses work on the SPU */
static inline uint32_t spu_ld32(const void *p) { const unsigned short *h = (const unsigned short *)p; return (uint32_t)GETLE16(h) | ((uint32_t)GETLE16(h + 1) << 16); }
static inline void spu_st32(void *p, uint32_t v) { unsigned short *h = (unsigned short *)p; PUTLE16(h, v & 0xffff); PUTLE16(h + 1, v >> 16); }
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
#define dshift 10

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
static short DrawSemiTrans;
static BOOL bCheckMask;
static unsigned short sSetMask;
static uint32_t lSetMask;
static int iDither;
static int32_t GlobalTextAddrX, GlobalTextAddrY, GlobalTextABR;
static short g_m1, g_m2, g_m3;

#define TEXB_SRC(e) TEXB(YAdjust + (e))
/* TEXB(byteOffset) / TEXW(wordIndex) come from the includer */

#include "spu_soft_gen.inc"

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
    const short *x = p->x, *y = p->y, *tx = p->tx, *ty = p->ty;
    short clX = p->clx, clY = p->cly;
    const int *c = p->col;
    lx0 = x[0]; ly0 = y[0]; lx1 = x[1]; ly1 = y[1]; lx2 = x[2]; ly2 = y[2]; lx3 = x[3]; ly3 = y[3];

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
