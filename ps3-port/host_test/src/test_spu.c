#include "SoftGPU/gpulib_if.c"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
volatile unsigned long ws_bc[8], ws_step;
int ws_hires = 0;
int ws_spu_render_on;
volatile int ws_spu_busy;
void ws_spu_flush(void){}
void ws_spu_set_vram(void*v){}

char interlacedMode;
spu_prim_t caplist[4096]; int ncap;
void ws_spu_cap_add(const spu_prim_t *p) { if (ncap < 4096) caplist[ncap++] = *p; }
void ws_spu_sync(void) {}
extern int sr_oob;
extern void host_render(const spu_prim_t *list, unsigned n, void *vram, void *dest, unsigned hi, unsigned nw);

static uint32_t rs = 12345;
static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }
static int rr(int lo, int hi) { return lo + (int)(rnd() % (unsigned)(hi - lo + 1)); }

int main(int argc, char **argv)
{
    int iters = argc > 1 ? atoi(argv[1]) : 2000, it, bad = 0;
    unsigned short *A = malloc(1024 * 512 * 2), *B = malloc(1024 * 512 * 2), *V0 = malloc(1024 * 512 * 2);
    static const char *names[] = {"F3","G3","FT3","GT3","F4","FT4","GT4","SPR","TILE"};
    int stat[9] = {0}, statbad[9] = {0};
    psxDst = NULL; dshift = 10;
    int HI = getenv("HI") ? atoi(getenv("HI")) : 0;
    unsigned short *AH = malloc(2048 * 1024 * 2), *BH = malloc(2048 * 1024 * 2), *CH = malloc(2048 * 1024 * 2); static int hchg, hcap, htot;
    for (it = 0; it < iters; it++) {
        int kind = getenv("KIND") ? atoi(getenv("KIND")) : rr(0, 8), i;
        short x[4], y[4], tx[4], ty[4];
        int tp = rr(0, 2), clX, clY, cx, cy, sz;
        int32_t col[4]; int sw, sh, stx, sty; short tl[4]; uint32_t gd[3];
#define SPRCALL DrawSoftwareSprite((unsigned char *)gd, sw, sh, stx, sty)
        /* random vram: mix of noise and smooth */
        for (i = 0; i < 1024 * 512; i++) { unsigned v = rnd(); V0[i] = (rnd() & 3) ? (v & 0xffff) : 0; }
        memcpy(A, V0, 1024 * 512 * 2); memcpy(B, V0, 1024 * 512 * 2);
        /* state */
        psxVuw = A; psxVub = (unsigned char *)A; psxDst = A; dshift = 10;
        drawX = rr(0, 100); drawY = rr(0, 300); drawW = rr(150, 1023); drawH = rr(150, 511);
        if (rr(0, 9) == 0) { drawY = rr(0, 511); drawH = drawY + rr(0, 3); }
        DrawSemiTrans = rr(0, 1); bCheckMask = rr(0, 3) == 0; GlobalTextABR = rr(0, 3); iDither = rr(0, 2);
        sSetMask = rr(0, 3) == 0 ? 0x8000 : 0; lSetMask = ((uint32_t)sSetMask << 16) | sSetMask;
        GlobalTextAddrX = rr(0, 15) * 64; GlobalTextAddrY = 0;
        g_m1 = rr(64, 255); g_m2 = rr(64, 255); g_m3 = rr(64, 255); if (rr(0, 1)) g_m1 = g_m2 = g_m3 = 128;
        interlacedMode = rr(0, 3) == 0; PSXDisplay.Interlaced = 1; PSXDisplay.Disabled = 0; gInterlaceLine = rr(0, 1);
        cx = rr(-20, 1000); cy = rr(260, 500); sz = rr(4, 200);
        for (i = 0; i < 4; i++) { x[i] = cx + rr(0, sz); y[i] = cy + rr(0, sz); tx[i] = rr(0, 255); ty[i] = rr(0, 255); col[i] = rnd() & 0xffffff; }
        if (rr(0, 3) == 0) { for (i = 0; i < 4; i++) { tx[i] = rr(0, 60) + 100; ty[i] = rr(0, 60) + 100; } }
        clX = rr(0, 63) * 16; clY = rr(0, 255);
        lx0 = x[0]; ly0 = y[0]; lx1 = x[1]; ly1 = y[1]; lx2 = x[2]; ly2 = y[2]; lx3 = x[3]; ly3 = y[3];
        ncap = 0;
        if (kind >= 7) {   /* sprite / tile parameters */
            int w = rr(1, 120), h = rr(1, 100);
            sw = w; sh = h; stx = rr(0, 256 - (w > 256 ? 256 : w)); sty = rr(0, 255 - h);
            gd[2] = ((uint32_t)clY << 22) | ((uint32_t)(clX >> 4) << 16); ly0 = cy; lx0 = cx;
            tl[0] = cx; tl[1] = cy; tl[2] = cx + rr(0, 150); tl[3] = cy + rr(0, 100);
            PSXDisplay.DrawOffset.x = rr(-5, 5); PSXDisplay.DrawOffset.y = rr(-5, 5);
            GlobalTextTP = tp;
            if (kind == 8) { /* tile: pick clipped box the way the real routine does is done inside it */ }
        }
        if (HI && kind <= 6) {   /* draw at 2x into the shadow buffer, textures stay native */
            for (i = 0; i < 4; i++) { x[i] *= 2; y[i] *= 2; }
            drawX *= 2; drawY *= 2; drawW = drawW * 2 + 1; drawH = drawH * 2 + 1;
            lx0 = x[0]; ly0 = y[0]; lx1 = x[1]; ly1 = y[1]; lx2 = x[2]; ly2 = y[2]; lx3 = x[3]; ly3 = y[3];
            for (i = 0; i < 2048 * 1024; i++) AH[i] = BH[i] = CH[i] = (rnd() & 3) ? (rnd() & 0xffff) : 0;
            psxDst = AH; dshift = 11; interlacedMode = 0;
        }
        /* capture */
        spu_cap = 1; ws_spu_render_on = (kind >= 7);
        switch (kind) {
        case 0: drawPoly3F(col[0]); break;
        case 1: drawPoly3G(col[0], col[1], col[2]); break;
        case 4: drawPoly4F(col[0]); break;
        case 2: if (tp == 0) drawPoly3TEx4(x[0],y[0],x[1],y[1],x[2],y[2],tx[0],ty[0],tx[1],ty[1],tx[2],ty[2],clX,clY);
                else if (tp == 1) drawPoly3TEx8(x[0],y[0],x[1],y[1],x[2],y[2],tx[0],ty[0],tx[1],ty[1],tx[2],ty[2],clX,clY);
                else drawPoly3TD(x[0],y[0],x[1],y[1],x[2],y[2],tx[0],ty[0],tx[1],ty[1],tx[2],ty[2]); break;
        case 3: if (tp == 0) drawPoly3TGEx4(x[0],y[0],x[1],y[1],x[2],y[2],tx[0],ty[0],tx[1],ty[1],tx[2],ty[2],clX,clY,col[0],col[1],col[2]);
                else if (tp == 1) drawPoly3TGEx8(x[0],y[0],x[1],y[1],x[2],y[2],tx[0],ty[0],tx[1],ty[1],tx[2],ty[2],clX,clY,col[0],col[1],col[2]);
                else drawPoly3TGD(x[0],y[0],x[1],y[1],x[2],y[2],tx[0],ty[0],tx[1],ty[1],tx[2],ty[2],col[0],col[1],col[2]); break;
        case 5: if (tp == 0) drawPoly4TEx4(x[0],y[0],x[1],y[1],x[2],y[2],x[3],y[3],tx[0],ty[0],tx[1],ty[1],tx[2],ty[2],tx[3],ty[3],clX,clY);
                else if (tp == 1) drawPoly4TEx8(x[0],y[0],x[1],y[1],x[2],y[2],x[3],y[3],tx[0],ty[0],tx[1],ty[1],tx[2],ty[2],tx[3],ty[3],clX,clY);
                else drawPoly4TD(x[0],y[0],x[1],y[1],x[2],y[2],x[3],y[3],tx[0],ty[0],tx[1],ty[1],tx[2],ty[2],tx[3],ty[3]); break;
        case 6: if (tp == 0) drawPoly4TGEx4(x[0],y[0],x[1],y[1],x[2],y[2],x[3],y[3],tx[0],ty[0],tx[1],ty[1],tx[2],ty[2],tx[3],ty[3],clX,clY,col[0],col[1],col[2],col[3]);
                else if (tp == 1) drawPoly4TGEx8(x[0],y[0],x[1],y[1],x[2],y[2],x[3],y[3],tx[0],ty[0],tx[1],ty[1],tx[2],ty[2],tx[3],ty[3],clX,clY,col[0],col[1],col[2],col[3]);
                else drawPoly4TGD(x[0],y[0],x[1],y[1],x[2],y[2],x[3],y[3],tx[0],ty[0],tx[1],ty[1],tx[2],ty[2],tx[3],ty[3],col[0],col[1],col[2],col[3]); break;
        case 7: SPRCALL; break;
        case 8: FillSoftwareAreaTrans(tl[0], tl[1], tl[2], tl[3], (unsigned short)col[0]); break;
        }
        spu_cap = 0; ws_spu_render_on = 0; ws_spu_render_on = 0;
        /* reference: draw directly */
        {
            int nc = ncap; ncap = 0;
            switch (kind) {
            case 0: drawPoly3F(col[0]); break;
            case 1: drawPoly3G(col[0], col[1], col[2]); break;
            case 4: drawPoly4F(col[0]); break;
            case 2: if (tp == 0) drawPoly3TEx4(x[0],y[0],x[1],y[1],x[2],y[2],tx[0],ty[0],tx[1],ty[1],tx[2],ty[2],clX,clY);
                    else if (tp == 1) drawPoly3TEx8(x[0],y[0],x[1],y[1],x[2],y[2],tx[0],ty[0],tx[1],ty[1],tx[2],ty[2],clX,clY);
                    else drawPoly3TD(x[0],y[0],x[1],y[1],x[2],y[2],tx[0],ty[0],tx[1],ty[1],tx[2],ty[2]); break;
            case 3: if (tp == 0) drawPoly3TGEx4(x[0],y[0],x[1],y[1],x[2],y[2],tx[0],ty[0],tx[1],ty[1],tx[2],ty[2],clX,clY,col[0],col[1],col[2]);
                    else if (tp == 1) drawPoly3TGEx8(x[0],y[0],x[1],y[1],x[2],y[2],tx[0],ty[0],tx[1],ty[1],tx[2],ty[2],clX,clY,col[0],col[1],col[2]);
                    else drawPoly3TGD(x[0],y[0],x[1],y[1],x[2],y[2],tx[0],ty[0],tx[1],ty[1],tx[2],ty[2],col[0],col[1],col[2]); break;
            case 5: if (tp == 0) drawPoly4TEx4(x[0],y[0],x[1],y[1],x[2],y[2],x[3],y[3],tx[0],ty[0],tx[1],ty[1],tx[2],ty[2],tx[3],ty[3],clX,clY);
                    else if (tp == 1) drawPoly4TEx8(x[0],y[0],x[1],y[1],x[2],y[2],x[3],y[3],tx[0],ty[0],tx[1],ty[1],tx[2],ty[2],tx[3],ty[3],clX,clY);
                    else drawPoly4TD(x[0],y[0],x[1],y[1],x[2],y[2],x[3],y[3],tx[0],ty[0],tx[1],ty[1],tx[2],ty[2],tx[3],ty[3]); break;
            case 6: if (tp == 0) drawPoly4TGEx4(x[0],y[0],x[1],y[1],x[2],y[2],x[3],y[3],tx[0],ty[0],tx[1],ty[1],tx[2],ty[2],tx[3],ty[3],clX,clY,col[0],col[1],col[2],col[3]);
                    else if (tp == 1) drawPoly4TGEx8(x[0],y[0],x[1],y[1],x[2],y[2],x[3],y[3],tx[0],ty[0],tx[1],ty[1],tx[2],ty[2],tx[3],ty[3],clX,clY,col[0],col[1],col[2],col[3]);
                    else drawPoly4TGD(x[0],y[0],x[1],y[1],x[2],y[2],x[3],y[3],tx[0],ty[0],tx[1],ty[1],tx[2],ty[2],tx[3],ty[3],col[0],col[1],col[2],col[3]); break;
        case 7: SPRCALL; break;
        case 8: FillSoftwareAreaTrans(tl[0], tl[1], tl[2], tl[3], (unsigned short)col[0]); break;
            }
            ncap = nc;
        }
        sr_oob = 0; if (HI && kind <= 6) { memcpy(BH, AH, 0); } host_render(caplist, ncap, HI && kind <= 6 ? A : B, HI && kind <= 6 ? BH : B, HI && kind <= 6, getenv("NW") ? atoi(getenv("NW")) : 1 + rr(0, 4));
        stat[kind]++; { static int ch[9], cp[9]; if (memcmp(A, V0, 1024*512*2)) ch[kind]++; if (ncap) cp[kind]++; if (it == iters-1) printf("changed %d/%d captured %d (kind %d)\n", ch[kind], stat[kind], cp[kind], kind); }
        if (HI && kind <= 6) { htot++; if (ncap) hcap++; if (memcmp(AH, CH, 2048*1024*2)) hchg++; }
        { int hz = HI && kind <= 6; unsigned short *CA = hz ? AH : A, *CB = hz ? BH : B; int NN = hz ? 2048 * 1024 : 1024 * 512, WW = hz ? 2048 : 1024;
        if (memcmp(CA, CB, NN * 2)) {
            int firstbad = -1, cnt = 0;
            for (i = 0; i < NN; i++) if (CA[i] != CB[i]) { if (firstbad < 0) firstbad = i; cnt++; }
            if (getenv("DBGIT") && atoi(getenv("DBGIT"))==it) { printf("kind=%d tp=%d clip=%ld,%ld,%ld,%ld verts:",kind,tp,drawX,drawY,drawW,drawH); for(i=0;i<4;i++)printf(" (%d,%d)",x[i],y[i]); printf(" col0=%06x V0=%04x\n",col[0],V0[firstbad % 1024]); int k; for(k=0;k<ncap;k++)printf("cap pk=%d nv=%d y0=%d y1=%d il=%d\n",caplist[k].pk,caplist[k].nv,caplist[k].y0,caplist[k].y1,caplist[k].il);} if (bad < 12)
                printf("oob=%d MISMATCH it=%d kind=%s tp=%d semi=%d abr=%d mask=%d dith=%d il=%d diff=%d first=(%d,%d) ref=%04x spu=%04x ncap=%d\n", sr_oob, it, names[kind], tp, DrawSemiTrans, GlobalTextABR, bCheckMask, iDither, interlacedMode, cnt, firstbad % WW, firstbad / WW, CA[firstbad], CB[firstbad], ncap);
            bad++; statbad[kind]++;
        }
    }}
    if (HI) printf("hires: %d tests, %d captured, %d changed the buffer\n", htot, hcap, hchg);
    printf("done: %d iterations, %d mismatches\n", iters, bad);
    for (it = 0; it < 9; it++) printf("  %s: %d tests %d bad\n", names[it], stat[it], statbad[it]);
    return bad != 0;
}
struct psx_gpu gpu;
const unsigned char cmd_lengths[256];
