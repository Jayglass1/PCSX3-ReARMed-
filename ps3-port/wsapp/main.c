/* WiiStation PS3 - standalone PS3 homebrew host for the WiiStation core.
 * Video: RSX display buffers (CPU blit), audio: 48 kHz float port, input: ioPad. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <malloc.h>
#include <time.h>
#include <ppu-lv2.h>
#include <sysutil/sysutil.h>
#include <sysutil/video.h>
#include <sysmodule/sysmodule.h>
#include <rsx/gcm_sys.h>
#include <rsx/rsx.h>
#include <io/pad.h>
#include <audio/audio.h>
#include <sys/thread.h>
#include <sys/systime.h>
#include <sys/event_queue.h>
#include "libretro.h"
#include "font.h"

/* ---- core API (ps3_glue.c) ---- */
void retro_set_environment(retro_environment_t);
void retro_set_video_refresh(retro_video_refresh_t);
void retro_set_audio_sample(retro_audio_sample_t);
void retro_set_audio_sample_batch(retro_audio_sample_batch_t);
void retro_set_input_poll(retro_input_poll_t);
void retro_set_input_state(retro_input_state_t);
void retro_init(void);
bool retro_load_game(const struct retro_game_info *);
void retro_unload_game(void);
void retro_run(void);
void ps3_jit_selftest(void);
int ps3_jit_stage_test(int);
void ws_log(const char *fmt, ...);
void ws_probe(char *buf, int n);
void ws_set_skip(int on);
extern int ws_fskip_auto;
extern int ws_hires;
extern int ws_cycle_mult;
static const int clk_tbl[] = { 33, 50, 75, 100, 125, 150, 200 };
static int clk_idx = 3;
#define ws_clock_pct (clk_tbl[clk_idx])
void ws_hist_add(void);
void ws_hist_report(char *b, int n);
void ws_prof_str(char *b, int n, int runs);
extern unsigned long long ws_prof[6];
void ws_probe2(char *buf, int n);
static volatile unsigned run_count; static volatile int in_game;


#define BASE "/dev_hdd0/game/WSTN00001/USRDIR"
#define MARK "/dev_hdd0/tmp/wiistation_jit_pending"

static volatile int want_exit;
static int use_dynarec;

static void sysutil_cb(u64 st, u64 p, void *d)
{
    if (st == SYSUTIL_EXIT_GAME) want_exit = 1;
}

/* ---------------- video ---------------- */
#define CB_SIZE (0x100000)
#define HOST_SIZE (32 * 1024 * 1024)
typedef struct { u32 *ptr; u32 offset; } fbuf;
static gcmContextData *ctx;
static fbuf fbs[2];
static int cur;
static u16 sw, sh;
static u32 firstflip = 1;

static void wait_flip(void)
{
    while (gcmGetFlipStatus() != 0) usleep(200);
    gcmResetFlipStatus();
}

static int video_init(void)
{
    void *host = memalign(1024 * 1024, HOST_SIZE);
    videoState st; videoResolution res; videoConfiguration vc;
    rsxInit(&ctx, CB_SIZE, HOST_SIZE, host);
    if (!ctx) return 0;
    if (videoGetState(0, 0, &st) != 0 || st.state != 0) return 0;
    if (videoGetResolution(st.displayMode.resolution, &res) != 0) return 0;
    memset(&vc, 0, sizeof vc);
    vc.resolution = st.displayMode.resolution;
    vc.format = VIDEO_BUFFER_FORMAT_XRGB;
    vc.pitch = res.width * 4;
    vc.aspect = st.displayMode.aspect;
    if (videoConfigure(0, &vc, NULL, 0) != 0) return 0;
    gcmSetFlipMode(GCM_FLIP_VSYNC);
    sw = res.width; sh = res.height;
    for (int i = 0; i < 2; i++) {
        fbs[i].ptr = (u32 *)rsxMemalign(64, sw * sh * 4);
        if (!fbs[i].ptr) return 0;
        rsxAddressToOffset(fbs[i].ptr, &fbs[i].offset);
        gcmSetDisplayBuffer(i, fbs[i].offset, sw * 4, sw, sh);
        memset(fbs[i].ptr, 0, sw * sh * 4);
    }
    gcmResetFlipStatus();
    return 1;
}

extern volatile int ws_where;
static int flip_pending;
static void wait_prev_flip(void)
{
    if (flip_pending) { int w0 = ws_where; ws_where = 20; while (gcmGetFlipStatus() != 0) usleep(200); flip_pending = 0; ws_where = w0; }
}
static void present(void)
{
    wait_prev_flip();
    gcmResetFlipStatus();
    if (gcmSetFlip(ctx, cur) == 0) {
        rsxFlushBuffer(ctx);
        flip_pending = 1;
    }
    cur ^= 1;
}

static u32 *scanbuf; /* main-memory line buffer */

/* RSX hardware scaler: source frames live in RSX-mapped main memory */
#define SRC_BUF_SIZE (5 * 1024 * 1024)
static u8 *src_base; static u32 src_off; static int hw_ok;
void ws_set_fb(u32 *a, u32 *b);
static void hw_init(void)
{
    src_base = (u8 *)memalign(1024 * 1024, 2 * SRC_BUF_SIZE);
    if (!src_base) return;
    memset(src_base, 0, 2 * SRC_BUF_SIZE);
    if (gcmMapMainMemory(src_base, 2 * SRC_BUF_SIZE, &src_off) != 0) return;
    ws_set_fb((u32 *)src_base, (u32 *)(src_base + SRC_BUF_SIZE));
    hw_ok = 1;
}

/* ---------------- FXAA (RSX shader pass) ---------------- */
#include <rsx/rsx.h>
#include <rsx/rsx_program.h>
#include "fx/fx_vpo.h"
#include "fx/fx_fpo.h"
static int ws_fxaa = 1, fx_ok = 0;
static int ws_spu_pref = 1;
extern int ws_spu_render_on; extern int ws_spu_render_ready(void);
static rsxVertexProgram *fx_vp; static rsxFragmentProgram *fx_fp;
static void *fx_vpu, *fx_fpu; static u32 fx_fpoff;
static float *fx_vb; static u32 fx_vboff;
static u16 *fx_ib; static u32 fx_iboff;
static u32 fx_depth_off; static rsxProgramAttrib *fx_tex;
static void fx_init(void)
{
    u32 vps, fps;
    fx_vp = (rsxVertexProgram *)fx_vpo; fx_fp = (rsxFragmentProgram *)fx_fpo;
    rsxVertexProgramGetUCode(fx_vp, &fx_vpu, &vps);
    rsxFragmentProgramGetUCode(fx_fp, &fx_fpu, &fps);
    void *fb = rsxMemalign(64, fps);
    if (!fb) return;
    memcpy(fb, fx_fpu, fps);
    rsxAddressToOffset(fb, &fx_fpoff);
    fx_tex = rsxFragmentProgramGetAttrib(fx_fp, "tex");
    fx_vb = (float *)rsxMemalign(128, 4 * 7 * 4);
    fx_ib = (u16 *)rsxMemalign(128, 16);
    void *dp = rsxMemalign(64, sw * sh * 4);
    if (!fx_vb || !fx_ib || !dp || !fx_tex) return;
    rsxAddressToOffset(fx_vb, &fx_vboff);
    rsxAddressToOffset(fx_ib, &fx_iboff);
    rsxAddressToOffset(dp, &fx_depth_off);
    fx_ib[0] = 0; fx_ib[1] = 1; fx_ib[2] = 2; fx_ib[3] = 0; fx_ib[4] = 2; fx_ib[5] = 3;
    fx_ok = 1;
}
/* draws the source texture (main memory) into fbs[cur] with FXAA */
static void fx_run(u32 src_offset, unsigned w, unsigned h, size_t pitch,
                   unsigned ox, unsigned oy, unsigned dw, unsigned dh)
{
    gcmSurface sf; memset(&sf, 0, sizeof sf);
    sf.colorFormat = GCM_SURFACE_X8R8G8B8;
    sf.colorTarget = GCM_SURFACE_TARGET_0;
    sf.colorLocation[0] = GCM_LOCATION_RSX; sf.colorOffset[0] = fbs[cur].offset; sf.colorPitch[0] = sw * 4;
    for (int i = 1; i < 4; i++) { sf.colorLocation[i] = GCM_LOCATION_RSX; sf.colorOffset[i] = 0; sf.colorPitch[i] = 64; }
    sf.depthFormat = GCM_SURFACE_ZETA_Z24S8; sf.depthLocation = GCM_LOCATION_RSX;
    sf.depthOffset = fx_depth_off; sf.depthPitch = sw * 4;
    sf.type = GCM_SURFACE_TYPE_LINEAR; sf.antiAlias = GCM_SURFACE_CENTER_1;
    sf.width = sw; sf.height = sh; sf.x = 0; sf.y = 0;
    rsxSetSurface(ctx, &sf);

    float scale[4] = { sw * 0.5f, sh * -0.5f, 0.5f, 0.0f }, offs[4] = { sw * 0.5f, sh * 0.5f, 0.5f, 0.0f };
    rsxSetViewport(ctx, 0, 0, sw, sh, 0.0f, 1.0f, scale, offs);
    rsxSetScissor(ctx, 0, 0, sw, sh);
    rsxSetColorMask(ctx, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A);
    rsxSetColorMaskMrt(ctx, 0);
    rsxSetDepthTestEnable(ctx, GCM_FALSE);
    rsxSetDepthWriteEnable(ctx, GCM_FALSE);
    rsxSetBlendEnable(ctx, GCM_FALSE);
    rsxSetFrontFace(ctx, GCM_FRONTFACE_CCW);
    rsxSetCullFaceEnable(ctx, GCM_FALSE);

    /* quad in NDC covering the 4:3 area */
    float x0 = (float)ox / sw * 2.f - 1.f, x1 = (float)(ox + dw) / sw * 2.f - 1.f;
    float y0 = 1.f - (float)oy / sh * 2.f, y1 = 1.f - (float)(oy + dh) / sh * 2.f;
    float tx = 1.0f / w, ty = 1.0f / h;
    float v[4][7] = {
        { x0, y0, 0, 0, 0, tx, ty }, { x1, y0, 0, 1, 0, tx, ty },
        { x1, y1, 0, 1, 1, tx, ty }, { x0, y1, 0, 0, 1, tx, ty } };
    memcpy(fx_vb, v, sizeof v);
    __asm__ volatile("sync" : : : "memory");

    rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE);
    gcmTexture t; memset(&t, 0, sizeof t);
    t.format = GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN | GCM_TEXTURE_FORMAT_NRM;
    t.mipmap = 1; t.dimension = GCM_TEXTURE_DIMS_2D; t.cubemap = GCM_FALSE;
    t.remap = ((GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) |
               (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) |
               (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) |
               (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT) |
               (GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) |
               (GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
               (GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) |
               (GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT));
    t.width = w; t.height = h; t.depth = 1; t.location = GCM_LOCATION_CELL;
    t.pitch = pitch; t.offset = src_offset;
    u32 unit = fx_tex->index;
    rsxLoadTexture(ctx, unit, &t);
    rsxTextureControl(ctx, unit, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
    rsxTextureFilter(ctx, unit, 0, GCM_TEXTURE_LINEAR, GCM_TEXTURE_LINEAR, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
    rsxTextureWrapMode(ctx, unit, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);

    rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_POS, 0, fx_vboff, 28, 3, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX0, 0, fx_vboff + 12, 28, 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX1, 0, fx_vboff + 20, 28, 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxLoadVertexProgram(ctx, fx_vp, fx_vpu);
    rsxLoadFragmentProgramLocation(ctx, fx_fp, fx_fpoff, GCM_LOCATION_RSX);
    rsxSetUserClipPlaneControl(ctx, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
                               GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE);
    rsxDrawIndexArray(ctx, GCM_TYPE_TRIANGLES, fx_iboff, 6, GCM_INDEX_TYPE_16B, GCM_LOCATION_RSX);
    rsxSetWaitForIdle(ctx);
}
static int clear_pending;
static int ws_sharp = 0;
static int ws_analog = 1;
static void hw_blit(const u32 *src, unsigned w, unsigned h, size_t pitch)
{
    unsigned dh = sh, dw = dh * 4 / 3;
    if (dw > sw) { dw = sw; dh = dw * 3 / 4; }
    unsigned ox = (sw - dw) / 2, oy = (sh - dh) / 2;
    gcmTransferScale sc; gcmTransferSurface su;
    wait_prev_flip();
    if (clear_pending) {
        for (int i = 0; i < 2; i++) memset(fbs[i].ptr, 0, sw * sh * 4);
        clear_pending = 0;
    }
    /* make the freshly written source rows visible to the RSX */
    {
        for (unsigned y = 0; y < h; y++) {
            const char *a = (const char *)src + (size_t)y * pitch, *e = a + (size_t)w * 4;
            for (; a < e; a += 128) __asm__ volatile("dcbst 0,%0" : : "r"(a) : "memory");
        }
        __asm__ volatile("sync" : : : "memory");
    }
    if (ws_fxaa && !ws_sharp && fx_ok) {
        fx_run(src_off + (u32)((const u8 *)src - src_base), w, h, pitch, ox, oy, dw, dh);
        return;
    }
    memset(&sc, 0, sizeof sc); memset(&su, 0, sizeof su);
    sc.conversion = GCM_TRANSFER_CONVERSION_TRUNCATE;
    sc.format = GCM_TRANSFER_SCALE_FORMAT_X8R8G8B8;
    sc.operation = GCM_TRANSFER_OPERATION_SRCCOPY;
    sc.clipX = 0; sc.clipY = 0; sc.clipW = sw; sc.clipH = sh;
    sc.outX = ox; sc.outY = oy; sc.outW = dw; sc.outH = dh;
    sc.ratioX = rsxGetFixedSint32((float)w / (float)dw);
    sc.ratioY = rsxGetFixedSint32((float)h / (float)dh);
    sc.inW = w; sc.inH = h; sc.pitch = (u16)pitch;
    sc.origin = GCM_TRANSFER_ORIGIN_CORNER;
    sc.interp = ws_sharp ? GCM_TRANSFER_INTERPOLATOR_NEAREST : GCM_TRANSFER_INTERPOLATOR_LINEAR;
    sc.offset = src_off + (u32)((const u8 *)src - src_base);
    sc.inX = 0; sc.inY = 0;
    su.format = GCM_TRANSFER_SURFACE_FORMAT_A8R8G8B8;
    su.pitch = sw * 4;
    su.offset = fbs[cur].offset;
    rsxSetTransferScaleMode(ctx, GCM_TRANSFER_MAIN_TO_LOCAL, GCM_TRANSFER_SURFACE);
    rsxSetTransferScaleSurface(ctx, &sc, &su);
    rsxSetWaitForIdle(ctx);
}

static void blit_frame(const u32 *src, unsigned w, unsigned h, size_t pitch)
{
    if (!src || !w || !h) return;
    /* 4:3 picture fit into the screen, integer-free nearest scaling */
    unsigned dh = sh, dw = dh * 4 / 3;
    if (dw > sw) { dw = sw; dh = dw * 3 / 4; }
    unsigned ox = (sw - dw) / 2, oy = (sh - dh) / 2;
    wait_prev_flip();
    u32 *dst = fbs[cur].ptr;
    static unsigned short xmap[2048];
    static unsigned lastdw, lastw;
    if (lastdw != dw || lastw != w) {
        for (unsigned x = 0; x < dw && x < 2048; x++) xmap[x] = x * w / dw;
        lastdw = dw; lastw = w;
    }
    if (!scanbuf) scanbuf = (u32 *)memalign(128, 2048 * 4);
    unsigned prev = ~0u;
    for (unsigned y = 0; y < dh; y++) {
        unsigned sy = y * h / dh;
        u32 *d = dst + (oy + y) * sw + ox;
        if (sy != prev) {
            const u32 *s = (const u32 *)((const char *)src + sy * pitch);
            for (unsigned x = 0; x < dw; x++) scanbuf[x] = s[xmap[x]];
            prev = sy;
        }
        memcpy(d, scanbuf, dw * 4);
    }
}

static void clear_screen(u32 c)
{
    wait_prev_flip();
    u32 *d = fbs[cur].ptr;
    for (u32 i = 0; i < (u32)sw * sh; i++) d[i] = c;
}

static void draw_text(int x, int y, const char *s, u32 col, int scale)
{
    u32 *d = fbs[cur].ptr;
    for (; *s; s++, x += FW * scale) {
        int c = (unsigned char)*s;
        if (c < 32 || c > 126) continue;
        const unsigned char *g = font_data[c - 32];
        for (int r = 0; r < FH; r++)
            for (int b = 0; b < FW; b++)
                if (g[r] & (0x80 >> b))
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++) {
                            int px = x + b * scale + sx, py = y + r * scale + sy;
                            if (px < sw && py < sh) d[py * sw + px] = col;
                        }
    }
}

/* ---------------- audio ---------------- */
#define ABLOCK 256
#define ARING 16384
static audioPortConfig acfg;
static u32 aport;
static sys_event_queue_t aq;
static sys_ipc_key_t aqkey;
static sys_ppu_thread_t atid;
static volatile int aquit, aok;
static float ring[ARING * 2] __attribute__((aligned(16)));
static volatile u32 rwr, rrd;

static void feeder(void *arg)
{
    volatile u64 *ri = (volatile u64 *)(uintptr_t)acfg.readIndex;
    float *data = (float *)(uintptr_t)acfg.audioDataStart;
    u32 nb = (u32)acfg.numBlocks;
    static float stg[ABLOCK * 2] __attribute__((aligned(16)));
    u32 wr = (u32)((*ri + 1) % nb);
    int playing = 0;
    while (!aquit) {
        sys_event_t ev;
        if (sysEventQueueReceive(aq, &ev, 20 * 1000) != 0) continue;
        u32 r = (u32)(*ri % nb);
        u32 ahead = (wr + nb - r - 1) % nb;
        if (ahead > 3) { wr = (r + 1) % nb; ahead = 0; }
        while (ahead < 3) {
            u32 rd = rrd;
            /* keep a standing buffer: after an underrun wait until it has refilled */
            if (!playing && rwr - rd >= 3072) playing = 1;
            if (playing && rwr - rd < ABLOCK) playing = 0;
            if (playing) {
                __sync_synchronize();
                for (u32 i = 0; i < ABLOCK; i++) {
                    u32 s = ((rd + i) & (ARING - 1)) * 2;
                    stg[i * 2] = ring[s]; stg[i * 2 + 1] = ring[s + 1];
                }
                rrd = rd + ABLOCK;
            } else if (ahead == 0) memset(stg, 0, sizeof stg);
            else break;
            memcpy(data + wr * 2 * ABLOCK, stg, sizeof stg);
            wr = (wr + 1) % nb; ahead++;
        }
    }
    sysThreadExit(0);
}

static void audio_start(void)
{
    int r;
    sysModuleLoad(SYSMODULE_AUDIO);
    if (audioInit() != 0) return;
    audioPortParam p; memset(&p, 0, sizeof p);
    p.numChannels = AUDIO_PORT_2CH; p.numBlocks = AUDIO_BLOCK_8;
    p.attrib = AUDIO_PORT_INITLEVEL; p.level = 1.0f;
    if (audioPortOpen(&p, &aport) != 0) return;
    if (audioGetPortConfig(aport, &acfg) != 0) return;
    if (audioCreateNotifyEventQueue(&aq, &aqkey) != 0) return;
    if (audioSetNotifyEventQueue(aqkey) != 0) return;
    sysEventQueueDrain(aq);
    if (audioPortStart(aport) != 0) return;
    r = sysThreadCreate(&atid, feeder, NULL, 100, 0x4000, THREAD_JOINABLE, (char *)"ws_audio");
    aok = (r == 0);
}

/* SPU produces 44.1 kHz; the PS3 audio port runs at 48 kHz -> linear resample,
   with a small rate trim driven by ring fill to absorb clock drift. */
static void log_line(const char *s);
static size_t audio_batch(const int16_t *d, size_t frames)
{
    static double pos;            /* fractional read position in input frames (relative to d[-1]) */
    static float pl, pr;          /* previous input frame */
    if (!aok) return frames;
    u32 fill = rwr - rrd;
    {   static unsigned calls, tot, minf = 0xffffffff, maxf; static double sumtrim;
        calls++; tot += (unsigned)frames; if (fill < minf) minf = fill; if (fill > maxf) maxf = fill;
        sumtrim += (fill < 3072) ? -0.08 * (3072.0 - (double)fill) / 3072.0 : 0.0;
        if (calls >= 600) {
            char b[160]; snprintf(b, sizeof b, "audio: %u calls, %.1f frames/call, fill min %u max %u, avg trim %.1f%%", calls, (double)tot / calls, minf, maxf, 100.0 * sumtrim / calls);
            log_line(b); calls = 0; tot = 0; minf = 0xffffffff; maxf = 0; sumtrim = 0;
        }
    }
    double step = 44100.0 / 48000.0;
    {   /* when the emulator runs slower than real time, stretch the audio (slow-down) instead of crackling */
        /* only stretch when the buffer is really draining; short hiccups (disc reads) are absorbed */
        double trim = 0.0;
        if (fill < 3072) trim = -0.08 * (3072.0 - (double)fill) / 3072.0;
        else if (fill > 9000) trim = 0.03 * ((double)fill - 9000.0) / 4000.0;
        if (trim < -0.08) trim = -0.08;
        if (trim > 0.03) trim = 0.03;
        step *= 1.0 + trim;
    }
    /* input index i corresponds to d[i]; pos in [0,frames) with pl/pr as the sample at -1 */
    while (pos < (double)frames) {
        if (rwr - rrd >= ARING - 1) { pos += step; continue; }
        int i1 = (int)pos;              /* floor */
        double f = pos - i1;
        float l0, r0, l1, r1;
        l1 = d[i1 * 2] * (1.0f / 32768.0f); r1 = d[i1 * 2 + 1] * (1.0f / 32768.0f);
        if (i1 > 0) { l0 = d[(i1 - 1) * 2] * (1.0f / 32768.0f); r0 = d[(i1 - 1) * 2 + 1] * (1.0f / 32768.0f); }
        else { l0 = pl; r0 = pr; }
        /* interpolate between sample (i1-1) and i1 using f in [0,1) over that interval */
        u32 s = (rwr & (ARING - 1)) * 2;
        ring[s] = l0 + (l1 - l0) * (float)f;
        ring[s + 1] = r0 + (r1 - r0) * (float)f;
        __sync_synchronize();
        rwr++;
        pos += step;
    }
    pos -= (double)frames;
    if (frames) { pl = d[(frames - 1) * 2] * (1.0f / 32768.0f); pr = d[(frames - 1) * 2 + 1] * (1.0f / 32768.0f); }
    return frames;
}
static void audio_one(int16_t l, int16_t r) { int16_t b[2] = { l, r }; audio_batch(b, 1); }

/* ---------------- input ---------------- */
static padData pd[MAX_PORT_NUM];
static padInfo pinfo;

static void poll_pads(void)
{
    ioPadGetInfo(&pinfo);
    for (int i = 0; i < MAX_PORT_NUM; i++)
        if (pinfo.status[i]) ioPadGetData(i, &pd[i]);
        else memset(&pd[i], 0, sizeof pd[i]);
}
static void in_poll(void) { poll_pads(); }

static int16_t in_state(unsigned port, unsigned dev, unsigned idx, unsigned id)
{
    if (port >= MAX_PORT_NUM) return 0;
    padData *d = &pd[port];
    if (dev == RETRO_DEVICE_JOYPAD) {
        switch (id) {
        case RETRO_DEVICE_ID_JOYPAD_B: return d->BTN_CROSS;
        case RETRO_DEVICE_ID_JOYPAD_A: return d->BTN_CIRCLE;
        case RETRO_DEVICE_ID_JOYPAD_Y: return d->BTN_SQUARE;
        case RETRO_DEVICE_ID_JOYPAD_X: return d->BTN_TRIANGLE;
        case RETRO_DEVICE_ID_JOYPAD_L: return d->BTN_L1;
        case RETRO_DEVICE_ID_JOYPAD_R: return d->BTN_R1;
        case RETRO_DEVICE_ID_JOYPAD_L2: return d->BTN_L2;
        case RETRO_DEVICE_ID_JOYPAD_R2: return d->BTN_R2;
        case RETRO_DEVICE_ID_JOYPAD_L3: return d->BTN_L3;
        case RETRO_DEVICE_ID_JOYPAD_R3: return d->BTN_R3;
        case RETRO_DEVICE_ID_JOYPAD_START: return d->BTN_START;
        case RETRO_DEVICE_ID_JOYPAD_SELECT: return d->BTN_SELECT;
        case RETRO_DEVICE_ID_JOYPAD_UP: return d->BTN_UP;
        case RETRO_DEVICE_ID_JOYPAD_DOWN: return d->BTN_DOWN;
        case RETRO_DEVICE_ID_JOYPAD_LEFT: return d->BTN_LEFT;
        case RETRO_DEVICE_ID_JOYPAD_RIGHT: return d->BTN_RIGHT;
        }
        return 0;
    }
    if (dev == RETRO_DEVICE_ANALOG) {
        int v;
        if (idx == RETRO_DEVICE_INDEX_ANALOG_LEFT)
            v = (id == RETRO_DEVICE_ID_ANALOG_X) ? d->ANA_L_H : d->ANA_L_V;
        else
            v = (id == RETRO_DEVICE_ID_ANALOG_X) ? d->ANA_R_H : d->ANA_R_V;
        v = (v - 128) * 258;
        if (v > 32767) v = 32767;
        if (v < -32767) v = -32767;
        return (int16_t)v;
    }
    return 0;
}

/* ---------------- libretro-style environment ---------------- */
static bool env_cb(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char **)data = BASE "/system"; return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char **)data = BASE "/saves"; return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = (struct retro_variable *)data;
        if (!strcmp(v->key, "wiistation_cpu")) { v->value = use_dynarec ? "Dynarec" : "Interpreter"; return true; }
        if (!strcmp(v->key, "wiistation_pad")) { v->value = ws_analog ? "Analog" : "Digital"; return true; }
        return false;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool *)data = false; return true;
    default: return false;
    }
}

static int frame_have; static unsigned lw, lh, lpx;
static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (!data) return; /* dupe frame: keep previous contents */
    lw = w; lh = h; lpx = ((const u32 *)data)[(h / 2) * (pitch / 4) + w / 2];
    if (hw_ok && (const u8 *)data >= src_base && (const u8 *)data < src_base + 2 * SRC_BUF_SIZE)
        hw_blit((const u32 *)data, w, h, pitch);
    else
        blit_frame((const u32 *)data, w, h, pitch);
    frame_have = 1;
}

static void watchdog(void *arg)
{
    unsigned last = ~0u; int still = 0;
    for (;;) {
        sleep(2);
        if (!in_game) { last = ~0u; still = 0; continue; }
        unsigned rc = run_count;
        char b[300], l[400];
        ws_probe(b, sizeof b);
        snprintf(l, sizeof l, "watch: runs=%u %s%s", rc, b, rc == last ? "  <-- NO PROGRESS" : "");
        if (rc == last) { if (++still > 8) continue; } else { still = 0; last = rc; continue; }
        ws_log("%s\n", l);
        if (rc == last && still == 1) { ws_probe2(b, sizeof b); ws_log("%s\n", b); }
        last = rc;
    }
}

/* ---------------- game list ---------------- */
#define MAXG 512
static char *games[MAXG];
static int ngames;

static int has_ext(const char *n, const char *e)
{
    size_t a = strlen(n), b = strlen(e);
    return a > b && !strcasecmp(n + a - b, e);
}
static int have_cue(const char *dir, const char *n)
{
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, n);
    size_t l = strlen(p);
    if (l < 4) return 0;
    strcpy(p + l - 3, "cue");
    struct stat st; if (!stat(p, &st)) return 1;
    strcpy(p + l - 3, "CUE");
    return !stat(p, &st);
}
static void scan(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && ngames < MAXG) {
        if (e->d_name[0] == '.') continue;
        char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 2) scan(p, depth + 1); continue; }
        const char *n = e->d_name;
        int ok = has_ext(n, ".cue") || has_ext(n, ".chd") || has_ext(n, ".iso") || has_ext(n, ".pbp") ||
                 has_ext(n, ".img") || has_ext(n, ".mdf") || has_ext(n, ".exe") ||
                 (has_ext(n, ".bin") && !have_cue(dir, n));
        if (ok) games[ngames++] = strdup(p);
    }
    closedir(d);
}
static int cmp(const void *a, const void *b) { return strcasecmp(*(char **)a, *(char **)b); }
static void scan_all(void)
{
    static const char *dirs[] = { BASE "/roms", "/dev_hdd0/PSXISO", "/dev_hdd0/ROMS/psx", "/dev_hdd0/ROMS/PSX",
                                  "/dev_hdd0/roms/psx", "/dev_usb000/PSXISO", "/dev_usb000/roms", "/dev_usb001/PSXISO", NULL };
    for (int i = 0; dirs[i]; i++) scan(dirs[i], 0);
    qsort(games, ngames, sizeof *games, cmp);
}

/* ---------------- main ---------------- */
static u32 prev_btn[2];
static u32 btns(int p)
{
    u32 b = 0;
    padData *d = &pd[p];
    if (d->BTN_UP) b |= 1; if (d->BTN_DOWN) b |= 2; if (d->BTN_CROSS) b |= 4; if (d->BTN_TRIANGLE) b |= 8;
    if (d->BTN_START) b |= 16; if (d->BTN_SELECT) b |= 32; if (d->BTN_L1) b |= 64; if (d->BTN_R1) b |= 128; if (d->BTN_SQUARE) b |= 256; if (d->BTN_CIRCLE) b |= 512; if (d->BTN_L2) b |= 1024; if (d->BTN_R2) b |= 2048; if (d->BTN_LEFT) b |= 4096; if (d->BTN_RIGHT) b |= 8192;
    return b;
}
static u32 pressed(void)
{
    u32 n = 0, i;
    for (i = 0; i < 2; i++) { u32 b = btns(i); n |= b & ~prev_btn[i]; prev_btn[i] = b; }
    return n;
}

static void log_line(const char *s)
{
    FILE *f = fopen("/dev_hdd0/tmp/wiistation_app.log", "a");
    if (f) { fputs(s, f); fputc('\n', f); fclose(f); }
}

static char stagemsg[200];
static int jit_state; /* 0 untested, 1 ok, -1 crashed previously */

static void menu(void)
{
    int sel = 0, top = 0;
    char line[1100];
    struct stat st;
    if (!stat(MARK, &st)) { jit_state = -1; unlink(MARK); }
    for (;;) {
        sysUtilCheckCallback();
        if (want_exit) return;
        poll_pads();
        u32 p = pressed();
        if ((p & 1) && sel > 0) sel--;
        if ((p & 2) && sel < ngames - 1) sel++;
        if (p & 64) sel = sel > 10 ? sel - 10 : 0;
        if (p & 128) sel = sel + 10 < ngames ? sel + 10 : (ngames ? ngames - 1 : 0);
        if (p & 8) {
            if (jit_state == -1) use_dynarec = 0;
            else use_dynarec ^= 1;
        }
        if (p & 256) ws_fskip_auto ^= 1;
        if (p & 32) ws_sharp ^= 1;
        if (p & 512) ws_hires = (ws_hires + 1) % 3;
        if (p & 1024) ws_analog ^= 1;
        if (p & 2048) ws_fxaa ^= 1;
        if (p & 16) ws_spu_pref ^= 1;
        if ((p & 4096) && clk_idx > 0) clk_idx--;
        if ((p & 8192) && clk_idx < 6) clk_idx++;
        ws_cycle_mult = 17500 / ws_clock_pct;
        if (p & 4 && ngames) {
            if (use_dynarec && jit_state == 0) {
                log_line("dynarec selected without test: running stage 3");
                FILE *f = fopen(MARK, "w"); if (f) fclose(f);
                int r = ps3_jit_stage_test(3);
                unlink(MARK);
                if (r != 42) use_dynarec = 0;
                jit_state = 1;
            }
            struct retro_game_info gi = { games[sel], NULL, 0, NULL };
            clear_screen(0); present();
            in_game = 1;
            if (!retro_load_game(&gi)) {
                in_game = 0;
                snprintf(line, sizeof line, "load failed: %s", games[sel]); log_line(line);
                continue;
            }
            /* run */
            u64 next_t = sysGetSystemTime(); int late_run = 0; u64 tr_run = 0, tr_pres = 0;
            clear_pending = 1; in_game = 1; int frames = 0; unsigned vf = 0, rf = 0; time_t t0 = time(NULL);
            for (;;) {
                sysUtilCheckCallback();
                if (want_exit) break;
                if (rf < 3) { snprintf(line, sizeof line, "calling retro_run #%u", rf + 1); log_line(line); }
                u64 tr0 = sysGetSystemTime();
                unsigned long long pg0 = ws_prof[0] + ws_prof[1], ps0 = ws_prof[2], pc0 = ws_prof[5], pe0 = ws_prof[3];
                extern unsigned long long ws_chd_io, ws_hcx[4]; unsigned long long q_io = ws_chd_io, q_h0 = ws_hcx[0], q_h1 = ws_hcx[1], q_h2 = ws_hcx[2], q_h3 = ws_hcx[3];
                ws_spu_render_on = ws_spu_pref && ws_spu_render_ready();
                ws_where = 1; retro_run(); ws_where = 2;
                u64 trd = sysGetSystemTime() - tr0;
                tr_run += trd;
                { static unsigned o17, o20; if (trd > 17500) o17++; if (trd > 20000) o20++;
                  if (((rf + 1) % 1800) == 0) { char ob[80]; snprintf(ob, sizeof ob, "slow runs this window: >17.5ms %u, >20ms %u", o17, o20); log_line(ob); o17 = o20 = 0; } }
                if (trd > 22000 && rf > 3 && ((rf % 1800) != 0)) {
                    static unsigned nst; if (nst++ < 60) {
                        char sb[400]; double k = 1000.0 / 79800000.0;
                        snprintf(sb, sizeof sb, "stall: run %u took %.1f ms (exec %.1f gpu %.1f spu %.1f chd %.1f)", rf, trd / 1000.0, (ws_prof[3] - pe0) * k,
                                 (ws_prof[0] + ws_prof[1] - pg0) * k, (ws_prof[2] - ps0) * k, (ws_prof[5] - pc0) * k); { extern unsigned long long ws_chd_io, ws_hcx[4]; char ib[120]; snprintf(ib, sizeof ib, "  io %.1f lock %.1f dec %.1f n %u pf %.1f", (ws_chd_io - q_io) * k, (ws_hcx[0] - q_h0) * k, (ws_hcx[1] - q_h1) * k, (unsigned)(ws_hcx[3] - q_h3), (ws_hcx[2] - q_h2) * k); strcat(sb, ib); }
                        log_line(sb);
                    }
                }
                rf++; run_count = rf;
                if (rf <= 3 || (rf % 1800) == 0) {
                    snprintf(line, sizeof line, "run: %u runs, %u video frames, %ld s, last %ux%u px0=%08x", rf, vf,
                             (long)(time(NULL) - t0), lw, lh, lpx);
                    log_line(line);
                    if (rf > 3) { char pb[600]; ws_prof_str(pb, sizeof pb, 1800);
                        snprintf(line, sizeof line, "%s | run=%.2fms present(total)=%.2fms/run", pb, tr_run / 1800000.0, tr_pres / 1800000.0);
                        log_line(line); tr_run = tr_pres = 0; }
                }
                ws_where = 3; if (frame_have) { u64 tp0 = sysGetSystemTime(); present(); tr_pres += sysGetSystemTime() - tp0; frame_have = 0; vf++; }
                ws_where = 4; poll_pads(); ws_where = 5;
                if (pd[0].BTN_SELECT && pd[0].BTN_START) break;
                {
                    u64 now = sysGetSystemTime();
                    next_t += 16683;
                    if (now < next_t) { ws_where = 6; ws_set_skip(0); usleep((unsigned)(next_t - now)); ws_where = 7; late_run = 0; }
                    else {
                        u64 late = now - next_t;
                        ws_set_skip(ws_fskip_auto && late > 2000);
                        if (late > 200000) next_t = now;
                    }
                }
            }
            in_game = 0;
            retro_unload_game();
            prev_btn[0] = prev_btn[1] = ~0u;
            if (want_exit) return;
            continue;
        }
        if (sel < top) top = sel;
        if (sel >= top + 11) top = sel - 10;
        clear_screen(0x00101830);
        draw_text(40, 20, "WiiStation PS3", 0x00FFFFFF, 3);
        snprintf(line, sizeof line, "CPU: %s%s   [Triangle] toggle", use_dynarec ? "Dynarec (experimental)" : "Interpreter",
                 jit_state == -1 ? "  (JIT crashed last time - disabled)" : "");
        draw_text(40, 70, line, 0x0080FF80, 2);
        snprintf(line, sizeof line, "Frameskip: %s   [Square] toggle   (Auto: keeps full game speed in heavy 3D scenes)", ws_fskip_auto ? "Auto" : "Off");
        draw_text(40, 96, line, 0x0080FF80, 2);
        snprintf(line, sizeof line, "Picture: %s   [Select] toggle   (Sharp = crisp pixels, Smooth = filtered)", ws_sharp ? "Sharp" : "Smooth");
        draw_text(40, 122, line, 0x0080FF80, 2);
        snprintf(line, sizeof line, "2x resolution: %s   [Circle] cycle   (low-res games only; Fast = quicker, rarely glitchy)", ws_hires == 2 ? "Fast" : ws_hires ? "On" : "Off");
        draw_text(40, 148, line, 0x0080FF80, 2);
        snprintf(line, sizeof line, "Controller: %s   [L2] toggle   (DualShock: games enable the sticks themselves)", ws_analog ? "DualShock" : "Digital");
        draw_text(40, 174, line, 0x0080FF80, 2);
        snprintf(line, sizeof line, "FXAA: %s   [R2] toggle   (smooths jagged edges; Smooth picture only)", ws_fxaa ? "On" : "Off");
        draw_text(40, 200, line, 0x0080FF80, 2);
        snprintf(line, sizeof line, "CPU clock: %d%%   [Left/Right] change   (below 100 = faster emulation, over 100 = fewer game slowdowns, slower)", ws_clock_pct);
        draw_text(40, 226, line, 0x0080FF80, 2);
        snprintf(line, sizeof line, "SPU renderer: %s   [Start] toggle   (draws 3D polygons on the Cell SPUs; faster fights)", ws_spu_pref ? "On" : "Off");
        draw_text(40, 252, line, 0x0080FF80, 2);
        if (!ngames) {
            draw_text(40, 170, "No games found. Put .cue/.bin, .chd, .iso or .pbp in:", 0x00FFFFFF, 2);
            draw_text(40, 230, BASE "/roms  or  /dev_hdd0/PSXISO", 0x00FFFF80, 2);
            draw_text(40, 260, "BIOS (optional): " BASE "/system/scph5501.bin", 0x00C0C0C0, 2);
        }
        for (int i = top; i < ngames && i < top + 11; i++) {
            const char *n = strrchr(games[i], '/'); n = n ? n + 1 : games[i];
            draw_text(40, 290 + (i - top) * 32, n, i == sel ? 0x00FFFF00 : 0x00FFFFFF, 2);
            if (i == sel) draw_text(16, 290 + (i - top) * 32, ">", 0x00FFFF00, 2);
        }
        draw_text(40, 110, stagemsg, 0x00FF8080, 2);
        draw_text(40, sh - 50, "X: start   Triangle: CPU mode   Select: picture   Circle: 2x   L2: pad   SELECT+START in game: back to list   PS button: quit", 0x00C0C0C0, 2);
        present();
    }
}

int main(int argc, char **argv)
{
    mkdir(BASE "/saves", 0777); mkdir(BASE "/system", 0777); mkdir(BASE "/roms", 0777);
    mkdir("/dev_hdd0/tmp", 0777);
    log_line("WiiStation PS3 standalone app start, build " __DATE__ " " __TIME__);
    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_cb, NULL);
    if (!video_init()) { log_line("video init failed"); return 1; }
    hw_init();
    fx_init();
    { char hl[80]; snprintf(hl, sizeof hl, "video: hardware scaler %s", hw_ok ? "on" : "OFF (cpu fallback)"); log_line(hl); }
    { extern int ws_spu_init(int); extern void ws_spu_selftest(void);
      if (ws_spu_init(5)) { ws_spu_selftest(); { extern int ws_spu_render_init(void); ws_spu_render_init(); } } }
    ioPadInit(MAX_PORT_NUM);
    audio_start();
    { sys_ppu_thread_t wt; sysThreadCreate(&wt, watchdog, NULL, 1500, 0x4000, THREAD_JOINABLE, (char *)"ws_watch"); }
    retro_set_environment(env_cb);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(audio_one);
    retro_set_audio_sample_batch(audio_batch);
    retro_set_input_poll(in_poll);
    retro_set_input_state(in_state);
    retro_init();
    scan_all();
    log_line("entering menu");
    menu();
    aquit = 1;
    ioPadEnd();
    return 0;
}
