/* RSX hardware renderer for the PS1 GPU.  #included into main.c (uses its ctx, fbs, fx_* and flip helpers).
 *
 * Design
 *  - The SoftGPU capture stream (spu_prim_t records, see wsps3/spu/spu_job.h) is replayed as triangles on the RSX.
 *  - VRAM lives in an N x N times larger A8R8G8B8 render target (RT), N = 1..4.  Alpha = PS1 mask bit.
 *    The 15 bit PS1 word is expanded 5 -> 8 bit with the usual replication so the shader can invert it exactly.
 *  - Texture sampling reads a second surface (CP) that is a copy of RT; only the dirty rectangle is copied, and only
 *    when a primitive's texture page / CLUT overlaps it (no read-while-render hazard).
 *  - The CPU keeps a normal 1x VRAM (psxVuw).  Rules, identical to the SPU renderer: anything on the CPU that READS
 *    VRAM first calls ws_spu_sync() -> ws_rsx_readback(); CPU WRITES (LoadImage, copies, CPU rasterised lines etc.)
 *    are found later by diffing psxVuw against a shadow copy and uploaded to RT and CP.
 *  - Display: the visible rectangle of RT is drawn into the flip buffer through the FXAA/scale shader.
 * Not supported (fall back to the CPU path or are ignored): texture window prims (CPU), mask bit test, dither,
 * interlace line skipping.
 */
#include "gp/gp_vpo.h"
#include "gp/gp_fpo.h"
#include "../wsps3/spu/spu_job.h"

extern unsigned short *psxVuw;
extern volatile int ws_spu_busy;
int ws_rsx_on;              /* renderer is active for the running game */
int ws_rsx_pref;            /* menu: 0 off, 1..4 = scale */
static int rx_scale, rx_ok, rx_inited;
static u32 rx_W, rx_H, rx_pitch;
static void *rx_rt_p, *rx_cp_p, *rx_z_p, *rx_vb_p;
static u32 rx_rt_off, rx_cp_off, rx_z_off, rx_vb_off;
static u8 *rx_up; static u32 rx_up_off; static u32 rx_up_pos;
#define RX_UP_SIZE (8u << 20)
static u8 *rx_rb; static u32 rx_rb_off; static u32 rx_rb_size;
static u16 *rx_shadow;
static rsxVertexProgram *rx_vp; static rsxFragmentProgram *rx_fp;
static void *rx_vpu, *rx_fpu; static u32 rx_fpoff; static u32 rx_texunit;
static int rx_bound, rx_texinval;
static int rx_cpu_maybe;
static int rx_dx0, rx_dy0, rx_dx1, rx_dy1;      /* drawn since the last readback (exclusive max) */
static int rx_cx0, rx_cy0, rx_cx1, rx_cy1;      /* drawn since the last RT -> CP copy */
static unsigned rx_frames, rx_nprims, rx_nbatch;
#define RX_LABEL 120
static u32 rx_label;

typedef struct { float x, y; u8 c[4]; float u, v; float pg[4]; float md[4]; float tw[4]; } rxv;   /* 68 bytes */
#define RX_STRIDE ((u32)sizeof(rxv))
#define RX_VBYTES (4u << 20)
#define RX_VMAX (RX_VBYTES / RX_STRIDE)
static u32 rx_vpos, rx_nv, rx_vstart;
typedef struct { u8 blend, amask; short sx, sy, sw, sh; } rxst;
static rxst rx_cur;

extern void ws_log(const char *fmt, ...);

static void rx_dirty_clear(void) { rx_dx0 = rx_dy0 = 1 << 20; rx_dx1 = rx_dy1 = -1; }
static void rx_cd_clear(void) { rx_cx0 = rx_cy0 = 1 << 20; rx_cx1 = rx_cy1 = -1; }

static int rx_wait_gpu(void)
{
    u64 t0 = sysGetSystemTime();
    rsxSetWriteBackendLabel(ctx, RX_LABEL, ++rx_label);
    rsxFlushBuffer(ctx);
    volatile u32 *l = gcmGetLabelAddress(RX_LABEL);
    while (*l != rx_label) {
        if (sysGetSystemTime() - t0 > 3000000) { ws_log("rsx: GPU did not finish within 3 s, renderer disabled\n"); rx_ok = 0; ws_rsx_on = 0; return 0; }
        usleep(50);
    }
    return 1;
}

static void rx_alloc_fail(const char *w) { ws_log("rsx: allocation failed (%s)\n", w); rx_ok = 0; }

/* one-time shader setup, then buffers for the chosen scale */
static int rx_init(int n)
{
    u32 vps, fps;
    if (!fx_ok) { ws_log("rsx: FXAA/scale pass unavailable\n"); return 0; }
    if (rx_ok && rx_scale == n) return 1;
    if (rx_ok) {
        rsxFree(rx_rt_p); rsxFree(rx_cp_p); rsxFree(rx_z_p);
        free(rx_rb); rx_ok = 0;
    }
    if (!rx_inited) {
        rx_vp = (rsxVertexProgram *)gp_vpo; rx_fp = (rsxFragmentProgram *)gp_fpo;
        rsxVertexProgramGetUCode(rx_vp, &rx_vpu, &vps);
        rsxFragmentProgramGetUCode(rx_fp, &rx_fpu, &fps);
        void *fb = rsxMemalign(64, fps);
        if (!fb) { rx_alloc_fail("fragment program"); return 0; }
        memcpy(fb, rx_fpu, fps);
        rsxAddressToOffset(fb, &rx_fpoff);
        rsxProgramAttrib *a = rsxFragmentProgramGetAttrib(rx_fp, "vram");
        if (!a) { ws_log("rsx: no vram sampler in fragment program\n"); return 0; }
        rx_texunit = a->index;
        rx_vb_p = rsxMemalign(128, RX_VBYTES);
        if (!rx_vb_p) { rx_alloc_fail("vertex ring"); return 0; }
        rsxAddressToOffset(rx_vb_p, &rx_vb_off);
        rx_up = (u8 *)memalign(1024 * 1024, RX_UP_SIZE);
        if (!rx_up || gcmMapMainMemory(rx_up, RX_UP_SIZE, &rx_up_off) != 0) { rx_alloc_fail("upload staging"); return 0; }
        rx_shadow = (u16 *)calloc(1024 * 512, 2);
        if (!rx_shadow) { rx_alloc_fail("shadow"); return 0; }
        rx_inited = 1;
    }
    rx_scale = n; rx_W = 1024 * n; rx_H = 512 * n; rx_pitch = rx_W * 4;
    rx_rt_p = rsxMemalign(128, rx_pitch * rx_H);
    rx_cp_p = rsxMemalign(128, rx_pitch * rx_H);
    rx_z_p = rsxMemalign(128, rx_W * 2 * rx_H);
    if (!rx_rt_p || !rx_cp_p || !rx_z_p) { rx_alloc_fail("render target"); return 0; }
    rsxAddressToOffset(rx_rt_p, &rx_rt_off); rsxAddressToOffset(rx_cp_p, &rx_cp_off); rsxAddressToOffset(rx_z_p, &rx_z_off);
    rx_rb_size = (u32)(rx_pitch * rx_H);                          /* whole target worst case: 1024N x 512N x 4 (32 MB at 4x) */
    rx_rb_size = (rx_rb_size + 0xfffff) & ~0xfffffu;
    rx_rb = (u8 *)memalign(1024 * 1024, rx_rb_size);
    if (!rx_rb || gcmMapMainMemory(rx_rb, rx_rb_size, &rx_rb_off) != 0) { rx_alloc_fail("readback staging"); return 0; }
    rx_ok = 1;
    ws_log("rsx: renderer ready, %dx scale (%ux%u target)\n", n, rx_W, rx_H);
    return 1;
}

static void rx_bind_rt(void)
{
    if (rx_bound) return;
    gcmSurface sf; memset(&sf, 0, sizeof sf);
    sf.colorFormat = GCM_SURFACE_A8R8G8B8;
    sf.colorTarget = GCM_SURFACE_TARGET_0;
    sf.colorLocation[0] = GCM_LOCATION_RSX; sf.colorOffset[0] = rx_rt_off; sf.colorPitch[0] = rx_pitch;
    for (int i = 1; i < 4; i++) { sf.colorLocation[i] = GCM_LOCATION_RSX; sf.colorOffset[i] = 0; sf.colorPitch[i] = 64; }
    sf.depthFormat = GCM_SURFACE_ZETA_Z16; sf.depthLocation = GCM_LOCATION_RSX;
    sf.depthOffset = rx_z_off; sf.depthPitch = rx_W * 2;
    sf.type = GCM_SURFACE_TYPE_LINEAR; sf.antiAlias = GCM_SURFACE_CENTER_1;
    sf.width = rx_W; sf.height = rx_H; sf.x = 0; sf.y = 0;
    rsxSetSurface(ctx, &sf);
    float scale[4] = { rx_W * 0.5f, rx_H * -0.5f, 0.5f, 0.0f }, offs[4] = { rx_W * 0.5f, rx_H * 0.5f, 0.5f, 0.0f };
    rsxSetViewport(ctx, 0, 0, rx_W, rx_H, 0.0f, 1.0f, scale, offs);
    rsxSetColorMask(ctx, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A);
    rsxSetColorMaskMrt(ctx, 0);
    rsxSetDepthTestEnable(ctx, GCM_FALSE);
    rsxSetDepthWriteEnable(ctx, GCM_FALSE);
    rsxSetFrontFace(ctx, GCM_FRONTFACE_CCW);
    rsxSetCullFaceEnable(ctx, GCM_FALSE);
    rsxLoadVertexProgram(ctx, rx_vp, rx_vpu);
    rsxLoadFragmentProgramLocation(ctx, rx_fp, rx_fpoff, GCM_LOCATION_RSX);
    rsxSetUserClipPlaneControl(ctx, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
                               GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE);
    rx_bound = 1; rx_texinval = 1;
}

static void rx_bind_tex(void)
{
    gcmTexture t; memset(&t, 0, sizeof t);
    t.format = GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN | GCM_TEXTURE_FORMAT_UNRM;
    t.mipmap = 1; t.dimension = GCM_TEXTURE_DIMS_2D; t.cubemap = GCM_FALSE;
    t.remap = ((GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) |
               (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) |
               (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) |
               (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT) |
               (GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) |
               (GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
               (GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) |
               (GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT));
    t.width = rx_W; t.height = rx_H; t.depth = 1; t.location = GCM_LOCATION_RSX;
    t.pitch = rx_pitch; t.offset = rx_cp_off;
    rsxLoadTexture(ctx, rx_texunit, &t);
    rsxTextureControl(ctx, rx_texunit, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
    rsxTextureFilter(ctx, rx_texunit, 0, GCM_TEXTURE_NEAREST, GCM_TEXTURE_NEAREST, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
    rsxTextureWrapMode(ctx, rx_texunit, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
}

static void rx_flush(void)
{
    if (!rx_nv || !rx_ok) { rx_nv = 0; return; }
    rx_bind_rt();
    if (rx_texinval) { rsxSetWaitForIdle(ctx); rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE); rx_bind_tex(); rx_texinval = 0; }
    rsxSetScissor(ctx, rx_cur.sx * rx_scale, rx_cur.sy * rx_scale, rx_cur.sw * rx_scale, rx_cur.sh * rx_scale);
    switch (rx_cur.blend) {
    case 0: rsxSetBlendEnable(ctx, GCM_FALSE); break;
    case 1: /* B/2 + F/2 */
        rsxSetBlendEnable(ctx, GCM_TRUE);
        rsxSetBlendFunc(ctx, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA, rx_cur.amask ? GCM_ONE : GCM_ZERO, rx_cur.amask ? GCM_ZERO : GCM_ONE);
        rsxSetBlendEquation(ctx, GCM_FUNC_ADD, GCM_FUNC_ADD); break;
    case 2: /* B + F  (and B + F/4, quartered in the shader) */
        rsxSetBlendEnable(ctx, GCM_TRUE);
        rsxSetBlendFunc(ctx, GCM_ONE, GCM_ONE, rx_cur.amask ? GCM_ONE : GCM_ZERO, rx_cur.amask ? GCM_ZERO : GCM_ONE);
        rsxSetBlendEquation(ctx, GCM_FUNC_ADD, GCM_FUNC_ADD); break;
    default: /* B - F */
        rsxSetBlendEnable(ctx, GCM_TRUE);
        rsxSetBlendFunc(ctx, GCM_ONE, GCM_ONE, rx_cur.amask ? GCM_ONE : GCM_ZERO, rx_cur.amask ? GCM_ZERO : GCM_ONE);
        rsxSetBlendEquation(ctx, GCM_FUNC_REVERSE_SUBTRACT, GCM_FUNC_ADD); break;
    }
    u32 base = rx_vb_off + rx_vstart * RX_STRIDE;
    __asm__ volatile("sync" : : : "memory");
    rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_POS,    0, base +  0, RX_STRIDE, 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_COLOR0, 0, base +  8, RX_STRIDE, 4, GCM_VERTEX_DATA_TYPE_U8,  GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX0,   0, base + 12, RX_STRIDE, 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX1,   0, base + 20, RX_STRIDE, 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX2,   0, base + 36, RX_STRIDE, 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX3,   0, base + 52, RX_STRIDE, 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxDrawVertexArray(ctx, GCM_TYPE_TRIANGLES, 0, rx_nv);
    rx_vstart += rx_nv; rx_nv = 0; rx_nbatch++;
}

/* RT -> CP for the rectangle drawn since the last copy */
static void rx_copy_dirty(void)
{
    if (rx_cx1 <= rx_cx0 || !rx_ok) return;
    rx_flush();
    rsxSetWaitForIdle(ctx);
    rsxSetTransferImage(ctx, GCM_TRANSFER_LOCAL_TO_LOCAL, rx_cp_off, rx_pitch, rx_cx0 * rx_scale, rx_cy0 * rx_scale,
                        rx_rt_off, rx_pitch, rx_cx0 * rx_scale, rx_cy0 * rx_scale,
                        (rx_cx1 - rx_cx0) * rx_scale, (rx_cy1 - rx_cy0) * rx_scale, 4);
    rsxSetWaitForIdle(ctx);
    rx_texinval = 1;
    rx_cd_clear();
}

/* 1x CPU pixels (psxVuw rect) -> RT and CP at N x, nearest */
static void rx_upload(int x, int y, int w, int h)
{
    if (!rx_ok || w <= 0 || h <= 0) return;
    rx_flush();
    rsxSetWaitForIdle(ctx);
    u32 pitch = ((u32)w * 4 + 63) & ~63u, bytes = pitch * h;
    if (rx_up_pos + bytes > RX_UP_SIZE) { if (!rx_wait_gpu()) return; rx_up_pos = 0; }
    u32 *dst = (u32 *)(rx_up + rx_up_pos);
    for (int j = 0; j < h; j++) {
        const u16 *s = psxVuw + (y + j) * 1024 + x;
        u32 *d = (u32 *)((u8 *)dst + (size_t)j * pitch);
        for (int i = 0; i < w; i++) {
            u32 c = s[i], r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
            d[i] = ((c & 0x8000) ? 0xff000000u : 0) | (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2));
        }
        for (u8 *a = (u8 *)d; a < (u8 *)(d + w); a += 128) __asm__ volatile("dcbst 0,%0" : : "r"(a) : "memory");
    }
    __asm__ volatile("sync" : : : "memory");
    for (int k = 0; k < 2; k++) {
        gcmTransferScale sc; gcmTransferSurface su;
        memset(&sc, 0, sizeof sc); memset(&su, 0, sizeof su);
        sc.conversion = GCM_TRANSFER_CONVERSION_TRUNCATE;
        sc.format = GCM_TRANSFER_SCALE_FORMAT_A8R8G8B8;
        sc.operation = GCM_TRANSFER_OPERATION_SRCCOPY;
        sc.clipX = 0; sc.clipY = 0; sc.clipW = rx_W; sc.clipH = rx_H;
        sc.outX = x * rx_scale; sc.outY = y * rx_scale; sc.outW = w * rx_scale; sc.outH = h * rx_scale;
        sc.ratioX = rsxGetFixedSint32(1.0f / rx_scale); sc.ratioY = rsxGetFixedSint32(1.0f / rx_scale);
        sc.inW = w; sc.inH = h; sc.pitch = (u16)pitch;
        sc.origin = GCM_TRANSFER_ORIGIN_CORNER;
        sc.interp = GCM_TRANSFER_INTERPOLATOR_NEAREST;
        sc.offset = rx_up_off + rx_up_pos;
        sc.inX = 0; sc.inY = 0;
        su.format = GCM_TRANSFER_SURFACE_FORMAT_A8R8G8B8;
        su.pitch = rx_pitch;
        su.offset = k ? rx_cp_off : rx_rt_off;
        rsxSetTransferScaleMode(ctx, GCM_TRANSFER_MAIN_TO_LOCAL, GCM_TRANSFER_SURFACE);
        rsxSetTransferScaleSurface(ctx, &sc, &su);
    }
    rsxSetWaitForIdle(ctx);
    rx_texinval = 1;
    rx_up_pos += (bytes + 127) & ~127u;
}

/* find what the CPU changed in psxVuw since the shadow copy and send it to the RSX */
static void rx_cpu_sync_up(void)
{
    if (!rx_cpu_maybe || !rx_ok) return;
    rx_cpu_maybe = 0;
    int y0 = -1, y1 = -1, x0 = 1024, x1 = -1;
    const u64 *a = (const u64 *)psxVuw, *b = (const u64 *)rx_shadow;
    for (int y = 0; y < 512; y++) {
        const u64 *ra = a + y * 256, *rb = b + y * 256;
        int f = -1, l = -1;
        for (int i = 0; i < 256; i++) if (ra[i] != rb[i]) { if (f < 0) f = i; l = i; }
        if (f < 0) continue;
        if (y0 < 0) y0 = y;
        y1 = y;
        if (f * 4 < x0) x0 = f * 4;
        if (l * 4 + 3 > x1) x1 = l * 4 + 3;
    }
    if (y0 < 0) return;
    rx_flush();
    rx_upload(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    for (int y = y0; y <= y1; y++) memcpy(rx_shadow + y * 1024 + x0, psxVuw + y * 1024 + x0, (size_t)(x1 - x0 + 1) * 2);
}

/* CPU is about to read VRAM: bring the drawn area back to 1x */
void ws_rsx_readback(void)
{
    if (!ws_rsx_on || !rx_ok) { ws_spu_busy = 0; return; }
    rx_cpu_maybe = 1;
    if (rx_dx1 > rx_dx0 && rx_dy1 > rx_dy0) {
        int x0 = rx_dx0, y0 = rx_dy0, w = rx_dx1 - rx_dx0, h = rx_dy1 - rx_dy0;
        rx_flush();
        rsxSetWaitForIdle(ctx);
        u32 rbp = ((u32)w * rx_scale * 4 + 63) & ~63u;
        rsxSetTransferImage(ctx, GCM_TRANSFER_LOCAL_TO_MAIN, rx_rb_off, rbp, 0, 0, rx_rt_off, rx_pitch,
                            x0 * rx_scale, y0 * rx_scale, w * rx_scale, h * rx_scale, 4);
        if (rx_wait_gpu()) {
            int c = rx_scale / 2;
            for (int j = 0; j < h; j++) {
                const u32 *row = (const u32 *)(rx_rb + (size_t)(j * rx_scale + c) * rbp);
                for (const u8 *p = (const u8 *)row; p < (const u8 *)row + (size_t)w * rx_scale * 4; p += 128) __asm__ volatile("dcbf 0,%0" : : "r"(p) : "memory");
                __asm__ volatile("sync" : : : "memory");
                u16 *d = psxVuw + (y0 + j) * 1024 + x0;
                for (int i = 0; i < w; i++) {
                    u32 px = row[i * rx_scale + c];
                    d[i] = (u16)(((px >> 19) & 31) | (((px >> 11) & 31) << 5) | (((px >> 3) & 31) << 10) | ((px >> 24) ? 0x8000 : 0));
                }
                memcpy(rx_shadow + (y0 + j) * 1024 + x0, d, (size_t)w * 2);
            }
        }
        rx_dirty_clear();
    }
    ws_spu_busy = 0;
}

static inline void rx_union(int *x0, int *y0, int *x1, int *y1, int ax0, int ay0, int ax1, int ay1)
{
    if (ax0 < *x0) *x0 = ax0; if (ay0 < *y0) *y0 = ay0; if (ax1 > *x1) *x1 = ax1; if (ay1 > *y1) *y1 = ay1;
}

static void rx_vert(rxv *v, float x, float y, const u8 *col, float u, float vv, const float *pg, const float *md)
{
    v->x = x; v->y = y;
    v->c[0] = col[0]; v->c[1] = col[1]; v->c[2] = col[2]; v->c[3] = 255;
    v->u = u; v->v = vv;
    memcpy(v->pg, pg, 16); memcpy(v->md, md, 16);
    v->tw[0] = v->tw[1] = v->tw[2] = v->tw[3] = 0;
}

static void rx_setstate(u8 blend, u8 amask, int sx, int sy, int sw_, int sh_)
{
    if (rx_nv && (rx_cur.blend != blend || rx_cur.amask != amask || rx_cur.sx != sx || rx_cur.sy != sy || rx_cur.sw != sw_ || rx_cur.sh != sh_)) rx_flush();
    rx_cur.blend = blend; rx_cur.amask = amask; rx_cur.sx = sx; rx_cur.sy = sy; rx_cur.sw = sw_; rx_cur.sh = sh_;
}

static rxv *rx_room(unsigned n)
{
    if (rx_vpos + n > RX_VMAX) {
        rx_flush();
        if (!rx_wait_gpu()) return NULL;
        rx_vpos = 0; rx_vstart = 0;
    }
    rxv *v = (rxv *)((u8 *)rx_vb_p + (size_t)rx_vpos * RX_STRIDE);
    rx_vpos += n; rx_nv += n;
    return v;
}

static inline void rx_word_rgb(unsigned c, u8 *o)
{
    unsigned r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    o[0] = (r << 3) | (r >> 2); o[1] = (g << 3) | (g >> 2); o[2] = (b << 3) | (b >> 2);
}

/* one primitive; called from ws_spu_cap_add() in the GPU code */
void ws_rsx_prim(const spu_prim_t *p)
{
    if (!rx_ok) return;
    rx_cpu_sync_up();
    int textured = (p->pk == 2 || p->pk == 3 || p->pk == 4);
    float px[4], py[4], tu[4], tv[4];
    u8 col[4][4];
    int nv = p->nv, i;
    /* geometry, in the order of the triangles below */
    if (p->pk == 4) {
        float sx = (float)(p->x[0] + p->x[1]), sy = (float)(p->y[0] + p->y[1]), w = p->tx[1], h = p->ty[1];
        px[0] = sx; py[0] = sy; px[1] = sx + w; py[1] = sy; px[2] = sx; py[2] = sy + h; px[3] = sx + w; py[3] = sy + h;
        tu[0] = p->tx[0]; tv[0] = p->ty[0]; tu[1] = tu[0] + w; tv[1] = tv[0]; tu[2] = tu[0]; tv[2] = tv[0] + h; tu[3] = tu[1]; tv[3] = tv[2];
        nv = 4;
        for (i = 0; i < 4; i++) { col[i][0] = p->gm[0]; col[i][1] = p->gm[1]; col[i][2] = p->gm[2]; }
    } else if (p->pk == 5) {
        px[0] = p->x[0]; py[0] = p->y[0]; px[1] = p->x[1]; py[1] = p->y[0]; px[2] = p->x[0]; py[2] = p->y[1]; px[3] = p->x[1]; py[3] = p->y[1];
        tu[0] = tu[1] = tu[2] = tu[3] = tv[0] = tv[1] = tv[2] = tv[3] = 0;
        nv = 4;
        for (i = 0; i < 4; i++) rx_word_rgb((unsigned)p->col[0] & 0xffff, col[i]);
    } else {
        /* untextured quads are in command order (A B C D); textured quads have POSITIONS/UVs in the cyclic order
         * A B D C but colours still in command order: bring positions to A B C D */
        int map[4] = { 0, 1, 2, 3 }, cmap[4] = { 0, 1, 2, 3 };
        if (nv == 4 && p->pk >= 2) { map[2] = 3; map[3] = 2; }
        for (i = 0; i < nv; i++) {
            int s = map[i];
            px[i] = p->x[s]; py[i] = p->y[s]; tu[i] = p->tx[s]; tv[i] = p->ty[s];
            if (p->pk == 2) { col[i][0] = p->gm[0]; col[i][1] = p->gm[1]; col[i][2] = p->gm[2]; }
            else if (p->pk == 0) { unsigned c = (unsigned)p->col[0]; col[i][0] = c; col[i][1] = c >> 8; col[i][2] = c >> 16; }
            else { unsigned c = (unsigned)p->col[cmap[i]]; col[i][0] = c; col[i][1] = c >> 8; col[i][2] = c >> 16; }
        }
    }
    /* bounding box in VRAM pixels, clipped to the draw area */
    float fx0 = px[0], fx1 = px[0], fy0 = py[0], fy1 = py[0];
    for (i = 1; i < nv; i++) { if (px[i] < fx0) fx0 = px[i]; if (px[i] > fx1) fx1 = px[i]; if (py[i] < fy0) fy0 = py[i]; if (py[i] > fy1) fy1 = py[i]; }
    int cx0 = p->clip[0] < 0 ? 0 : p->clip[0], cy0 = p->clip[1] < 0 ? 0 : p->clip[1];
    int cx1 = p->clip[2] > 1023 ? 1023 : p->clip[2], cy1 = p->clip[3] > 511 ? 511 : p->clip[3];
    if (p->pk == 5) { cx0 = 0; cy0 = 0; cx1 = 1023; cy1 = 511; }
    if (cx1 < cx0 || cy1 < cy0) return;
    int bx0 = (int)fx0 < cx0 ? cx0 : (int)fx0, by0 = (int)fy0 < cy0 ? cy0 : (int)fy0;
    int bx1 = (int)fx1 + 1 > cx1 + 1 ? cx1 + 1 : (int)fx1 + 1, by1 = (int)fy1 + 1 > cy1 + 1 ? cy1 + 1 : (int)fy1 + 1;
    if (bx1 <= bx0 || by1 <= by0) return;

    float pg[4] = { 0, 0, 0, 0 };
    int tp = p->tp;
    if (textured) {
        int clx, cly;
        if (p->pk == 4) { clx = ((unsigned)p->col[0] >> 12) & 0x3f0; cly = ((unsigned)p->col[0] >> 22) & 511; }
        else { clx = p->clx; cly = p->cly; }
        pg[0] = p->tpx; pg[1] = p->tpy; pg[2] = clx; pg[3] = cly;
        /* texture source rectangle (page + CLUT) against what was drawn but not yet copied */
        int tw_ = tp == 0 ? 64 : tp == 1 ? 128 : 256, tx0 = p->tpx, ty0 = p->tpy;
        int hit = 0;
        if (rx_cx1 > rx_cx0) {
            int tx1 = tx0 + tw_; if (tx1 > 1024) { tx0 = 0; tx1 = 1024; }
            int ty1 = ty0 + 256; if (ty1 > 512) ty1 = 512;
            if (tx0 < rx_cx1 && tx1 > rx_cx0 && ty0 < rx_cy1 && ty1 > rx_cy0) hit = 1;
            if (tp < 2) {
                int ex = clx + (tp == 0 ? 16 : 256);
                if (clx < rx_cx1 && ex > rx_cx0 && cly < rx_cy1 && cly + 1 > rx_cy0) hit = 1;
            }
        }
        if (hit) rx_copy_dirty();
    }

    int semi = p->semi != 0;
    u8 amask = p->setmask != 0;
    unsigned abr = p->abr & 3;
    for (int pass = 0; pass < (textured && semi ? 2 : 1); pass++) {
        /* pass 0 of a semi textured prim: opaque texels without blending; pass 1: semi texels with blending */
        int blending = semi && (!textured || pass == (textured && semi ? 1 : 0));
        float tpass = !textured ? 0.f : (!semi ? 0.f : (pass == 0 ? 1.f : 2.f));
        u8 blend = 0;
        float flags = amask ? 4.f : 0.f;
        if (blending) {
            blend = abr == 0 ? 1 : abr == 2 ? 3 : 2;
            if (abr == 3) flags += 1.f;
            if (abr == 0) flags += 2.f;
        }
        float md[4] = { textured ? (float)(tp > 2 ? 2 : tp) : 3.f, tpass, (float)rx_scale, flags };
        rx_setstate(blend, amask, cx0, cy0, cx1 - cx0 + 1, cy1 - cy0 + 1);
        unsigned nvert = nv == 4 ? 6 : 3;
        rxv *v = rx_room(nvert);
        if (!v) return;
        if (nv == 4) {
            static const int ix[6] = { 0, 1, 2, 1, 3, 2 };
            for (i = 0; i < 6; i++) { int k = ix[i]; rx_vert(&v[i], px[k], py[k], col[k], tu[k], tv[k], pg, md); }
        } else {
            for (i = 0; i < 3; i++) rx_vert(&v[i], px[i], py[i], col[i], tu[i], tv[i], pg, md);
        }
    }
    rx_union(&rx_dx0, &rx_dy0, &rx_dx1, &rx_dy1, bx0, by0, bx1, by1);
    rx_union(&rx_cx0, &rx_cy0, &rx_cx1, &rx_cy1, bx0, by0, bx1, by1);
    ws_spu_busy = 1;
    rx_nprims++;
}

/* the CPU wrote (or may write) VRAM */
void ws_rsx_note_cpu(void) { rx_cpu_maybe = 1; }

/* fresh game: black VRAM everywhere */
void ws_rsx_reset(void)
{
    if (!rx_ok) return;
    rx_nv = 0; rx_vpos = 0; rx_vstart = 0; rx_up_pos = 0; rx_bound = 0; rx_texinval = 1;
    memset(rx_shadow, 0, 1024 * 512 * 2);
    rx_dirty_clear(); rx_cd_clear(); rx_cpu_maybe = 1;
    rx_bind_rt();
    rsxSetClearColor(ctx, 0);
    rsxSetScissor(ctx, 0, 0, rx_W, rx_H);
    rsxClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
    rsxSetWaitForIdle(ctx);
    rsxSetTransferImage(ctx, GCM_TRANSFER_LOCAL_TO_LOCAL, rx_cp_off, rx_pitch, 0, 0, rx_rt_off, rx_pitch, 0, 0, rx_W, rx_H, 4);
    rsxSetWaitForIdle(ctx);
    rx_wait_gpu();
}

/* display: draw the visible VRAM rectangle (native pixels) into the flip buffer */
static void fx_run_rt(int sx, int sy, int w, int h)
{
    unsigned dh = sh, dw = dh * 4 / 3;
    if (dw > sw) { dw = sw; dh = dw * 3 / 4; }
    unsigned ox = (sw - dw) / 2, oy = (sh - dh) / 2;
    float n = (float)rx_scale, iw = 1.0f / rx_W, ih = 1.0f / rx_H;
    /* FXAA span 0 = passthrough (fx_span[0]) */
    fx_run_ex(GCM_LOCATION_RSX, rx_rt_off, rx_W, rx_H, rx_pitch, ox, oy, dw, dh,
              sx * n * iw, sy * n * ih, (sx + w) * n * iw, (sy + h) * n * ih);
}
void ws_rsx_present(int sx, int sy, int w, int h)
{
    if (!rx_ok) return;
    rx_cpu_sync_up();
    rx_flush();
    rsxSetWaitForIdle(ctx);
    rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE);
    wait_prev_flip();
    if (clear_pending) {
        for (int i = 0; i < 2; i++) memset(fbs[i].ptr, 0, sw * sh * 4);
        clear_pending = 0;
    }
    fx_run_rt(sx, sy, w, h);
    rx_bound = 0; rx_texinval = 1;
    rx_frames++;
    if (rx_frames <= 3 || (rx_frames % 1800) == 0) ws_log("rsx: frame %u prims %u batches %u (rect %d,%d %dx%d)\n", rx_frames, rx_nprims, rx_nbatch, sx, sy, w, h);
    rx_nprims = rx_nbatch = 0;
    frame_have = 1; lw = w; lh = h;
}
