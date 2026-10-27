/* shared PPU/SPU/host job interface for the SPU renderer helpers */
#ifndef SPU_JOB_H
#define SPU_JOB_H
#ifdef __SPU__
typedef unsigned int u32w; typedef unsigned long long u64w;
#else
#include <stdint.h>
typedef uint32_t u32w; typedef uint64_t u64w;
#endif

/* ---- SPU renderer interface ---- */
#define SJ_RENDER 2          /* draw a primitive list into this worker's bands of VRAM */
#define SPU_BAND_H 16        /* rows per band */
#define SPU_VSTRIDE 1024     /* pixels per VRAM row */

/* one primitive, 96 bytes (multiple of 16 so lists DMA cleanly) */
typedef struct {
    unsigned char  pk;          /* 0 F, 1 G, 2 FT, 3 GT */
    unsigned char  nv;          /* 3 or 4 */
    unsigned char  tp;          /* texture mode: 0 4bit, 1 8bit, 2 15bit */
    unsigned char  semi;        /* DrawSemiTrans */
    unsigned char  mask;        /* bCheckMask */
    unsigned char  abr;         /* GlobalTextABR */
    unsigned char  dither;      /* iDither */
    unsigned char  il;
    unsigned short setmask;     /* sSetMask */
    unsigned short tpx, tpy;    /* GlobalTextAddrX/Y */
    short          clip[4];     /* drawX, drawY, drawW, drawH */
    short          x[4], y[4];  /* vertices in the order the leaf routine takes them */
    short          tx[4], ty[4];
    short          clx, cly;
    short          gm[3];       /* g_m1..3 */
    short          y0, y1;      /* covered rows, for band culling */
    int            col[4];      /* colour words in call-argument order */
    int            pad1[3];
} spu_prim_t;

#define SJ_QUIT  0xffffffffu
#define SJ_TEST  1

/* one control block per worker, 128-byte aligned, lives in main memory */
typedef struct {
    u32w cmd;          /* job command */
    u32w seq;          /* sequence number the PPU waits for */
    u32w done;         /* set to seq by the SPU when finished */
    u32w worker;       /* worker index */
    u64w ea_a, ea_b;   /* buffers: a = prim list, b = VRAM */
    u32w n_a, n_b;     /* a = prim count, b = number of workers */
    u32w res[8];       /* results / profile */
    u32w padx[2];
    u32w dbg[4];       /* SPU progress breadcrumbs (16-byte aligned: offset 80) */
    u32w pad[8];
    u32w kt[12], kc[12];  /* per primitive kind: ticks, prim-band count */
    u32w ph[8];
    u32w kp[12], ks[12];  /* per kind: bbox pixels in the band, rows stepped above the band */
} __attribute__((aligned(128))) spu_ctl_t;

#endif
