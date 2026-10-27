/* PS3 / libretro glue for the WiiStation (PCSX-Reloaded lineage) core.
 * Replaces the Wii frontend: video, audio, pad, disc and memory card I/O. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <ppu-types.h>
#include <sys/thread.h>
#include "libretro.h"

#include "psxcommon.h"
#include "r3000a.h"
#include "psxmem.h"
#include "plugins.h"
#include "cdriso.h"
#include "misc.h"
#include "sio.h"
#include "gpu.h"
#include "spu.h"
#include "psxcounters.h"
#include "psemu_plugin_defs.h"
#include "decode_xa.h"
#include "gpulib/plugin_lib.h"
#include "Gamecube/wiiSXconfig.h"
#include "Gamecube/MEM2.h"
#include "Gamecube/GamecubePlugins.h"
#include "Gamecube/fileBrowser/fileBrowser.h"
#include "dfsound/out.h"

#ifndef CALLBACK
#define CALLBACK
#endif

void ps3_jit_log(const char *fmt, ...);

/* ------------------------------------------------------------------ */
/* memory buffers that used to live in Wii MEM2                         */
char ps3_mcd1_buf[128*1024];
char ps3_mcd2_buf[128*1024];
char ps3_spu_buf[512*1024];
char ps3_rec_data[4*1024*1024] __attribute__((aligned(128)));
char pSpuBuffer[WII_SPU_FREQ];
char XABuf[WII_SPU_FREQ * sizeof(uint32_t) * 2];

/* ------------------------------------------------------------------ */
/* config globals (wiiSXconfig.h)                                       */
char audioEnabled = 1, spuInterpolation = SIMPLE_INTERPOLATION, showFPSonScreen = 0,
     printToScreen = 0, printToSD = 0, frameLimit[2] = {0, 1}, frameSkip = 0,
     useDithering = 0, saveEnabled = 0, nativeSaveDevice = 0, saveStateDevice = 0,
     autoSave = 1, creditsScrolling = 0, dynacore = DYNACORE_INTERPRETER,
     biosDevice = BIOSDEVICE_HLE, LoadCdBios = 0, screenMode = 0, videoMode = 0,
     fileSortMode = 1, padAutoAssign = 1, padType[10], padAssign[10], padLightgun[10],
     rumbleEnabled = 0, loadButtonSlot = 0, controllerType = 0, numMultitaps = 0,
     lang = 0, fastLoad = 0, originalMode = 0, bilinearFilter = 1, trapFilter = 0,
     interlacedMode = 0, deflickerFilter = 0, lightGun = 0, memCard[2] = {1, 1},
     forceNTSC = 0, gpuPlugin = NEW_SOFT;

int stop = 0;
int backFromMenu = 0;
int g_emu_resetting = 0;
bool needInitCpu = true;
signed char autoSaveLoaded = 0;
char fpsInfo[32];
char txtbuffer[1024];
PcsxConfig Config;
PadDataS pad, lastport1, lastport2;
R3000Acpu psxLightrec;       /* Lightrec is not part of this port */
int lightrec_mmap(void *p, unsigned a, unsigned l) { return 0; }


/* ------------------------------------------------------------------ */
/* cache helpers used by the dynarec                                    */
void DCFlushRange(void *ptr, u32 len) { }
void DCStoreRange(void *ptr, u32 len) { }
void DCInvalidateRange(void *ptr, u32 len) { }
void ICInvalidateRange(void *ptr, u32 len)
{
    uintptr_t a = (uintptr_t)ptr & ~(uintptr_t)127, e = (uintptr_t)ptr + len;
    for (uintptr_t p = a; p < e; p += 128) __asm__ volatile("dcbst 0,%0" :: "r"(p) : "memory");
    __asm__ volatile("sync" ::: "memory");
    for (uintptr_t p = a; p < e; p += 128) __asm__ volatile("icbi 0,%0" :: "r"(p) : "memory");
    __asm__ volatile("sync; isync" ::: "memory");
}

/* ------------------------------------------------------------------ */
/* misc stubs the core expects                                          */
void DEBUG_print(char *s, int pos) { }
void DEBUG_stats(int a, char *b, unsigned c, unsigned d) { }
void DEBUG_update(void) { }
char **DEBUG_get_text(void) { return NULL; }
void printFunctionName(void) { }
void LoadingBar_showBar(float p, const char *s) { }
void pauseRemovalThread(void) { }
void continueRemovalThread(void) { }
void lightgunInterrupt(void) { }
void SSS_SetMultiPad(int a, int b) { }
int lightrec_invalid_dummy;
void SysRunGui(void) { }
void ws_log(const char *fmt, ...)
{
    va_list a; char b[700]; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    FILE *f = fopen("/dev_hdd0/tmp/wiistation_app.log", "a");
    if (f) { fputs(b, f); fclose(f); }
}
void SysMessage(char *fmt, ...)
{
    va_list a; char b[512]; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    ws_log("SysMessage: %s\n", b);
}
void SysCloseLibrary(void *lib) { }
char *SysLibError(void) { return NULL; }
void SysPrintf(const char *fmt, ...)
{
    va_list a; char b[512]; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    ws_log("SysPrintf: %s", b);
}
void SysUpdate(void)
{
    stop = 1;      /* end of frame: let retro_run return */
}
void SysClose(void);
void *SysLoadLibrary(char *lib);
void *SysLoadSym(void *lib, char *sym);

/* ------------------------------------------------------------------ */
/* file layer                                                           */
fileBrowser_file *isoFile_topLevel, *saveFile_dir, *biosFile_dir;
fileBrowser_file isoFile, cddaFile, subFile;
fileBrowser_file *biosFile;
int (*isoFile_init)(fileBrowser_file *), (*isoFile_open)(fileBrowser_file *),
    (*isoFile_readDir)(fileBrowser_file *, fileBrowser_file **),
    (*isoFile_readFile)(fileBrowser_file *, void *, unsigned),
    (*isoFile_seekFile)(fileBrowser_file *, unsigned, unsigned),
    (*isoFile_deinit)(fileBrowser_file *);
int (*saveFile_init)(fileBrowser_file *), (*saveFile_readFile)(fileBrowser_file *, void *, unsigned),
    (*saveFile_writeFile)(fileBrowser_file *, void *, unsigned), (*saveFile_deinit)(fileBrowser_file *);
int (*biosFile_init)(fileBrowser_file *), (*biosFile_open)(fileBrowser_file *),
    (*biosFile_readFile)(fileBrowser_file *, void *, unsigned), (*biosFile_deinit)(fileBrowser_file *);

static int fb_nop(fileBrowser_file *f) { return 0; }
static int fb_read(fileBrowser_file *f, void *buf, unsigned len)
{
    FILE *fp = fopen(f->name, "rb");
    if (!fp) return FILE_BROWSER_ERROR_NO_FILE;
    fseek(fp, f->offset, SEEK_SET);
    size_t n = fread(buf, 1, len, fp);
    fclose(fp);
    f->offset += n;
    return (int)n;
}
static int fb_write(fileBrowser_file *f, void *buf, unsigned len)
{
    FILE *fp = fopen(f->name, f->offset ? "r+b" : "wb");
    if (!fp) return FILE_BROWSER_ERROR;
    fseek(fp, f->offset, SEEK_SET);
    size_t n = fwrite(buf, 1, len, fp);
    fclose(fp);
    f->offset += n;
    return (int)n;
}
static int fb_seek(fileBrowser_file *f, unsigned off, unsigned type)
{
    if (type == FILE_BROWSER_SEEK_SET) f->offset = off;
    else if (type == FILE_BROWSER_SEEK_CUR) f->offset += off;
    return f->offset;
}
static int fb_isoopen(fileBrowser_file *f) { return 0; }

/* ------------------------------------------------------------------ */
/* video                                                                */
static retro_environment_t env_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_audio_sample_t audio_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

#define FBW 1280
static uint32_t fb[2][FBW * 960];
static int out_w = 320, out_h = 240, frame_drawn;

static int vo_open(void) { return 0; }
static void vo_close(void) { }
static void vo_set_mode(int w, int h, int rw, int rh, int bpp) { out_w = w; out_h = h; }
static uint32_t *fbp[2];
static int fbi;
void ws_set_fb(uint32_t *a, uint32_t *b) { fbp[0] = a; fbp[1] = b; }
static void vo_flip(const void *vram, int stride, int bgr24, int x, int y, int w, int h, int dims)
{
    int i, j;
    uint32_t *o;
    if (w <= 0 || h <= 0 || w > FBW || h > 960) return;
    if (!fbp[0]) { fbp[0] = fb[0]; fbp[1] = fb[1]; }
    o = fbp[fbi]; fbi ^= 1;
    if (!vram) { for (j = 0; j < h; j++) memset(o + j * FBW, 0, w * 4); }
    else if (!bgr24) {
        static uint32_t lut[32768];
        static int lut_ok;
        const uint16_t *s = (const uint16_t *)vram;
        if (!lut_ok) {
            unsigned q;
            for (q = 0; q < 32768; q++) {
                unsigned r = q & 31, gg = (q >> 5) & 31, b = (q >> 10) & 31;
                r = (r << 3) | (r >> 2); gg = (gg << 3) | (gg >> 2); b = (b << 3) | (b >> 2);
                lut[q] = (r << 16) | (gg << 8) | b;
            }
            lut_ok = 1;
        }
        for (j = 0; j < h; j++, s += stride) {
            uint32_t *d = o + j * FBW;
            for (i = 0; i + 1 < w; i += 2) {
                unsigned a0 = __builtin_bswap16(s[i]) & 0x7fff, a1 = __builtin_bswap16(s[i + 1]) & 0x7fff;
                d[i] = lut[a0]; d[i + 1] = lut[a1];
            }
            if (i < w) d[i] = lut[__builtin_bswap16(s[i]) & 0x7fff];
        }
    } else {
        const uint8_t *s = (const uint8_t *)vram;
        for (j = 0; j < h; j++, s += stride * 2) {
            uint32_t *d = o + j * FBW;
            for (i = 0; i < w; i++)
                d[i] = (s[i * 3] << 16) | (s[i * 3 + 1] << 8) | s[i * 3 + 2];
        }
    }
    video_cb(o, w, h, FBW * 4);
    out_w = w; out_h = h; frame_drawn = 1;
}

extern u32 hSyncCount, frame_counter;
extern void gpu_state_change(int what);
struct rearmed_cbs pl_rearmed_cbs = {
    .gpu_state_change = gpu_state_change,
    .gpu_hcnt = &hSyncCount,
    .gpu_frame_count = &frame_counter,
    .pl_vout_open = vo_open,
    .pl_vout_set_mode = vo_set_mode,
    .pl_vout_flip = vo_flip,
    .pl_vout_close = vo_close,
};
extern void LIB_GPUrearmedCallbacks(const struct rearmed_cbs *cbs);
int ws_fskip_auto;
int ws_hires;
int ws_cycle_mult = 175;
int ws_hires_big;
void plugin_call_rearmed_cbs(unsigned long fixes, int dither)
{
    pl_rearmed_cbs.gpu_peops.dwActFixes = fixes;
    pl_rearmed_cbs.gpu_peops.iUseDither = dither;
    pl_rearmed_cbs.frameskip = ws_fskip_auto ? -1 : 0;
    pl_rearmed_cbs.gpu_neon.enhancement_enable = ws_hires != 0;
    pl_rearmed_cbs.gpu_peops.fFrameRateHz = Config.PsxType ? 50.0f : 60.0f;
    pl_rearmed_cbs.gpu_peops.dwFrameRateTicks = 0;
    LIB_GPUrearmedCallbacks(&pl_rearmed_cbs);
}
void pl_frame_limit(void) { }
void pl_timing_prepare(int is_pal) { }
void pl_chg_psxtype(int is_pal) { }
int gc_vout_open(void) { return 0; }

/* ------------------------------------------------------------------ */
/* audio                                                                */
static int16_t abuf[48000 / 30 * 2 + 4096];
static int apos;
static int snd_init(void) { apos = 0; return 0; }
static void snd_finish(void) { }
static int snd_busy(void) { return 1; }   /* never ask the SPU for extra samples: we run synchronously */
static int snd_feed(void *p, int bytes)
{
    int n = bytes / 2;
    if (apos + n > (int)(sizeof abuf / 2)) n = sizeof abuf / 2 - apos;
    memcpy(abuf + apos, p, n * 2);
    apos += n;
    return 0;
}
void out_register_sdl(struct out_driver *drv)
{
    drv->name = "libretro"; drv->init = snd_init; drv->finish = snd_finish;
    drv->busy = snd_busy; drv->feed = (void *)snd_feed;
}
int iDisStereo;

/* ------------------------------------------------------------------ */
/* pad (digital / analog)                                               */
static int pad_analog;



static unsigned short read_pad(int port, unsigned char *ax)
{
    static const int map[16] = {
        RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_L3, RETRO_DEVICE_ID_JOYPAD_R3,
        RETRO_DEVICE_ID_JOYPAD_START, RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_RIGHT,
        RETRO_DEVICE_ID_JOYPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_L2,
        RETRO_DEVICE_ID_JOYPAD_R2, RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R,
        RETRO_DEVICE_ID_JOYPAD_X, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B,
        RETRO_DEVICE_ID_JOYPAD_Y };
    unsigned short s = 0xffff;
    for (int i = 0; i < 16; i++)
        if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, map[i])) s &= ~(1 << i);
    for (int st = 0; st < 2; st++)
        for (int a = 0; a < 2; a++) {
            int v = input_state_cb(port, RETRO_DEVICE_ANALOG, st, a);
            { int q = (v >> 8) + 128; if (q > 116 && q < 140) q = 128; ax[st * 2 + a] = (unsigned char)(q < 0 ? 0 : q > 255 ? 255 : q); }
        }
    return s;
}
/* DualShock protocol: poll / config mode / analog mode switch */
static unsigned char pad_buf[2][8];
static int pad_cur, pad_curbyte, pad_cmdlen, pad_curcmd;
static int pad_conf[2], pad_mode[2];
static unsigned char pad_btn[2][2], pad_ax[2][4];

static unsigned char pad_do_cmd(void)
{
    unsigned char *b = pad_buf[pad_cur];
    int p = pad_cur;
    memset(b, 0, 8);
    b[1] = 0x5a;
    pad_cmdlen = 8;
    switch (pad_curcmd) {
    case 0x43:
        if (pad_conf[p]) { b[0] = 0xf3; return b[0]; }
        /* fall through: acts as a normal read until the param enters config */
    case 0x42:
        b[0] = pad_conf[p] ? 0xf3 : (pad_mode[p] ? 0x73 : 0x41);
        b[2] = pad_btn[p][0]; b[3] = pad_btn[p][1];
        if (pad_mode[p] || pad_conf[p]) {
            b[4] = pad_ax[p][2]; b[5] = pad_ax[p][3];
            b[6] = pad_ax[p][0]; b[7] = pad_ax[p][1];
        } else pad_cmdlen = 4;
        return b[0];
    case 0x44: b[0] = 0xf3; return b[0];
    case 0x45:
        b[0] = 0xf3; b[2] = 0x01; b[3] = 0x02; b[4] = pad_mode[p] ? 1 : 0; b[5] = 0x02; b[6] = 0x01;
        return b[0];
    case 0x46: b[0] = 0xf3; b[4] = 0x01; b[5] = 0x02; b[7] = 0x0a; return b[0];
    case 0x47: b[0] = 0xf3; b[4] = 0x02; b[6] = 0x01; return b[0];
    case 0x4c: b[0] = 0xf3; return b[0];
    case 0x4d: b[0] = 0xf3; b[1] = 0x5a; memset(b + 2, 0xff, 6); return b[0];
    default:   b[0] = 0xf3; return b[0];
    }
}
static unsigned char start_poll(int port)
{
    int p = port - 1;
    unsigned short s = read_pad(p, pad_ax[p]);
    pad_btn[p][0] = s & 0xff; pad_btn[p][1] = s >> 8;
    pad_cur = p; pad_curbyte = 0; pad_cmdlen = 0;
    return 0xff;
}
static unsigned char do_poll(unsigned char v)
{
    unsigned char *b = pad_buf[pad_cur];
    if (pad_curbyte == 0) {
        pad_curcmd = pad_analog ? v : 0x42;
        pad_curbyte = 1;
        { static unsigned char seen[256]; if (!seen[v]) { seen[v] = 1; ws_log("pad: first cmd %02x (conf=%d mode=%d)", v, pad_conf[pad_cur], pad_mode[pad_cur]); } }
        return pad_do_cmd();
    }
    if (pad_curbyte >= pad_cmdlen) return 0xff;
    if (pad_curbyte == 2) {
        switch (pad_curcmd) {
        case 0x43: if (pad_conf[pad_cur] != (v == 1)) ws_log("pad: config %d", v == 1); pad_conf[pad_cur] = (v == 1); break;
        case 0x44: if (pad_mode[pad_cur] != (v == 1)) ws_log("pad: analog mode %d", v == 1); pad_mode[pad_cur] = (v == 1); break;
        case 0x4c: b[5] = v ? 0x07 : 0x04; break;
        }
    }
    return b[pad_curbyte++];
}
static long PADinit_(long a) { return 0; }
static long PADshut_(void) { return 0; }
static long PADopen_(unsigned long *d) { return 0; }
static long PADclose_(void) { return 0; }
static long PADrp1(PadDataS *p) { memset(p, 0, sizeof *p); p->controllerType = PSE_PAD_TYPE_STANDARD; return 0; }
unsigned char PAD1_sp(int p) { return start_poll(1); }
unsigned char PAD2_sp(int p) { return start_poll(2); }
unsigned char PAD_pl(unsigned char v) { return do_poll(v); }
long PAD__init(long a) { return 0; }
long PAD__shutdown(void) { return 0; }
long SSS_PADopen(void *p) { return 0; }
long SSS_PADclose(void) { return 0; }
unsigned char SSS_PADstartPoll(int pad) { return start_poll(pad); }
unsigned char SSS_PADpoll(const unsigned char v) { return do_poll(v); }
long SSS_PADreadPort1(PadDataS *p) { return PADrp1(p); }
long SSS_PADreadPort2(PadDataS *p) { return PADrp1(p); }
long PAD1__open(void); long PAD2__open(void);

#define SYM(n, p) { n, (void *)(p) }
static PluginTable plugins_tbl[NUM_PLUGINS];
#define plugins plugins_tbl
void *SysLoadLibrary(char *lib)
{
    for (int i = 0; i < NUM_PLUGINS; i++)
        if (plugins[i].lib && !strcmp(lib, plugins[i].lib)) return (void *)(intptr_t)(i + 1);
    return NULL;
}
void *SysLoadSym(void *lib, char *sym)
{
    PluginTable *p = plugins + ((intptr_t)lib - 1);
    for (int i = 0; i < p->numSyms; i++)
        if (p->syms[i].sym && !strcmp(sym, p->syms[i].sym)) return p->syms[i].pntr;
    return NULL;
}
static void init_plugin_table(void)
{
    static const PluginTable spu = { "SPU", 15, {
        SYM("SPUinit", DF_SPUinit), SYM("SPUshutdown", DF_SPUshutdown), SYM("SPUopen", DF_SPUopen),
        SYM("SPUclose", DF_SPUclose), SYM("SPUwriteRegister", DF_SPUwriteRegister),
        SYM("SPUreadRegister", DF_SPUreadRegister), SYM("SPUwriteDMAMem", DF_SPUwriteDMAMem),
        SYM("SPUreadDMAMem", DF_SPUreadDMAMem), SYM("SPUplayADPCMchannel", DF_SPUplayADPCMchannel),
        SYM("SPUfreeze", DF_SPUfreeze), SYM("SPUregisterCallback", DF_SPUregisterCallback),
        SYM("SPUregisterScheduleCb", DF_SPUregisterScheduleCb), SYM("SPUasync", DF_SPUasync),
        SYM("SPUplayCDDAchannel", DF_SPUplayCDDAchannel), SYM("SPUsetCDvol", DF_SPUsetCDvol) } };
    plugins[0] = spu;
    for (int n = 1; n <= 2; n++) {
        PluginTable t = { n == 1 ? "PAD1" : "PAD2", 7, {
            SYM("PADinit", PAD__init), SYM("PADshutdown", PAD__shutdown), SYM("PADopen", SSS_PADopen),
            SYM("PADclose", SSS_PADclose), SYM("PADpoll", SSS_PADpoll), SYM("PADstartPoll", SSS_PADstartPoll),
            SYM(n == 1 ? "PADreadPort1" : "PADreadPort2", n == 1 ? SSS_PADreadPort1 : SSS_PADreadPort2) } };
        plugins[n] = t;
    }
}

/* ------------------------------------------------------------------ */
/* core lifecycle                                                       */
extern int SysInit(void);
int SysInit(void)
{
    Config.Cpu = dynacore;
    ws_log("SysInit: psxInit (cpu=%d)\n", Config.Cpu);
    psxInit();
    ws_log("SysInit: LoadPlugins\n");
    if (LoadPlugins() < 0) { ws_log("SysInit: LoadPlugins failed\n"); return -1; }
    ws_log("SysInit: OpenPlugins\n");
    if (OpenPlugins() < 0) { ws_log("SysInit: OpenPlugins failed\n"); return -1; }
    ws_log("SysInit: ok\n");
    return 0;
}
void SysReset(void)
{
    g_emu_resetting = 1;
    psxReset();
    g_emu_resetting = 0;
}
void SysClose(void)
{
    psxShutdown();
    ClosePlugins();
    ReleasePlugins();
}

static char sys_dir[512] = ".", save_dir[512] = ".", game_path[1024];
static bool game_loaded;

/* memory cards are written by a background thread so the HDD never stalls emulation */
static char mcd_stage[2][MCD_SIZE] __attribute__((aligned(16)));
static char mcd_last[2][MCD_SIZE];
static char mcd_lastvalid[2];
static char mcd_path[2][600];
static volatile int mcd_pending[2];
static sys_ppu_thread_t mcd_tid;
static int mcd_thread_up;

static void mcd_worker(void *a)
{
    for (;;) {
        for (int i = 0; i < 2; i++) {
            if (!mcd_pending[i]) continue;
            fileBrowser_file f;
            memcpy(&f, saveFile_dir, sizeof f);
            memset(f.name, 0, FILE_BROWSER_MAX_PATH_LEN);
            snprintf((char *)f.name, FILE_BROWSER_MAX_PATH_LEN, "%s", mcd_path[i]);
            int ok = saveFile_writeFile(&f, mcd_stage[i], MCD_SIZE) == MCD_SIZE;
            ws_log("memcard %d saved (%d)\n", i + 1, ok);
            mcd_pending[i] = 0;
        }
        usleep(30000);
    }
}

/* returns 1 when the card needs no further attention (queued or unchanged) */
static int mcd_queue(int n)
{
    char *src = n ? Mcd2Data : Mcd1Data;
    if (!saveFile_dir) return 1;
    if (mcd_pending[n]) return 0;
    if (mcd_lastvalid[n] && !memcmp(src, mcd_last[n], MCD_SIZE)) return 1;
    if (!mcd_thread_up) {
        if (sysThreadCreate(&mcd_tid, mcd_worker, NULL, 1600, 0x8000, THREAD_JOINABLE, (char *)"ws_mcd") != 0) return 1;
        mcd_thread_up = 1;
    }
    memcpy(mcd_stage[n], src, MCD_SIZE);
    memcpy(mcd_last[n], src, MCD_SIZE);
    mcd_lastvalid[n] = 1;
    if (n == 0) snprintf(mcd_path[0], sizeof mcd_path[0], "%s/%s.mcd", (char *)saveFile_dir->name, CdromId);
    else snprintf(mcd_path[1], sizeof mcd_path[1], "%s/slot2.mcd", (char *)saveFile_dir->name);
    __sync_synchronize();
    mcd_pending[n] = 1;
    return 1;
}

static void flush_cards(void)
{
    if (!saveFile_dir) return;
    for (int w = 0; w < 200 && (mcd_pending[0] || mcd_pending[1]); w++) usleep(10000);
    SaveMcd(1, saveFile_dir);
    SaveMcd(2, saveFile_dir);
    mcd_lastvalid[0] = mcd_lastvalid[1] = 0;
}

/* ------------------------------------------------------------------ */
/* libretro API                                                         */
void retro_set_environment(retro_environment_t cb)
{
    env_cb = cb;
    static const struct retro_variable vars[] = {
        { "wiistation_cpu", "CPU core; Dynarec (PPC)|Interpreter" },
        { "wiistation_pad", "Controller type; Digital|Analog" },
        { NULL, NULL } };
    cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void *)vars);
    bool nogame = false;
    (void)nogame;
}
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }
unsigned retro_api_version(void) { return RETRO_API_VERSION; }
void retro_set_controller_port_device(unsigned port, unsigned device) { }

void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof *info);
    info->library_name = "WiiStation";
    info->library_version = "ps3-port";
    info->valid_extensions = "bin|cue|img|iso|mdf|pbp|exe|toc|cbn|m3u|ccd|chd";
    info->need_fullpath = true;
}
void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof *info);
    info->geometry.base_width = 320; info->geometry.base_height = 240;
    info->geometry.max_width = 1024; info->geometry.max_height = 512;
    info->geometry.aspect_ratio = 4.0f / 3.0f;
    info->timing.fps = Config.PsxType ? 50.0 : 60.0;
    info->timing.sample_rate = WII_SPU_FREQ;
}

void retro_init(void)
{
    const char *d = NULL;
    if (env_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &d) && d) snprintf(sys_dir, sizeof sys_dir, "%s", d);
    if (env_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &d) && d) snprintf(save_dir, sizeof save_dir, "%s", d);
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
    init_plugin_table();
}
void retro_deinit(void) { }
void retro_reset(void) { if (game_loaded) SysReset(); }

static void update_variables(void)
{
    struct retro_variable v = { "wiistation_cpu", NULL };
    dynacore = DYNACORE_INTERPRETER;
    if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value && v.value[0] == 'D')
        dynacore = DYNACORE_DYNAREC_OLD;
    struct retro_variable p = { "wiistation_pad", NULL };
    padType[0] = 2; padType[1] = 0;
    pad_analog = env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &p) && p.value && p.value[0] == 'A';
}

extern void psxCpuInit(void);
void ps3_jit_selftest(void);
static void install_prof(void);
bool retro_load_game(const struct retro_game_info *g)
{
    static fileBrowser_file savedir, biosdir;
    if (!g || !g->path) return false;
    update_variables();

    isoFile_open = fb_isoopen; isoFile_init = fb_nop; isoFile_deinit = fb_nop;
    isoFile_readFile = fb_read; isoFile_seekFile = fb_seek;
    saveFile_init = fb_nop; saveFile_deinit = fb_nop; saveFile_readFile = fb_read; saveFile_writeFile = fb_write;
    biosFile_init = fb_nop; biosFile_open = fb_nop; biosFile_deinit = fb_nop; biosFile_readFile = fb_read;

    memset(&savedir, 0, sizeof savedir); snprintf(savedir.name, sizeof savedir.name, "%s", save_dir);
    saveFile_dir = &savedir;
    memset(&biosdir, 0, sizeof biosdir);
    biosFile_dir = &biosdir;

    memset(&Config, 0, sizeof Config);
    strcpy(Config.Net, "Disabled");
    Config.PsxOut = 0; Config.HLE = 1; Config.Xa = 0; Config.Cdda = 0;
    Config.cycle_multiplier = ws_cycle_mult;
    Config.PsxAuto = 1; Config.GpuListWalking = -1; Config.FractionalFramerate = -1;
    Config.Cpu = dynacore;
    gpuPtr = &newSoftGpu;

    /* BIOS: use a real one from the system dir when present */
    static const char *names[] = { "scph5501.bin", "scph1001.bin", "scph5500.bin", "scph5502.bin", "SCPH1001.BIN", "SCPH5501.BIN", NULL };
    static fileBrowser_file bf;
    memset(&bf, 0, sizeof bf);
    for (int i = 0; names[i]; i++) {
        char p[600]; snprintf(p, sizeof p, "%s/%s", sys_dir, names[i]);
        FILE *f = fopen(p, "rb");
        if (f) { fclose(f); snprintf(bf.name, sizeof bf.name, "%s", p); Config.HLE = 0; biosDevice = 1; break; }
    }
    if (Config.HLE) { biosDevice = BIOSDEVICE_HLE; snprintf(bf.name, sizeof bf.name, "%s/none.bin", sys_dir); }
    ws_log("bios: %s (HLE=%d device=%d)\n", bf.name, Config.HLE, (int)biosDevice);
    biosFile = &bf;

    memset(&isoFile, 0, sizeof isoFile);
    snprintf(isoFile.name, sizeof isoFile.name, "%s", g->path);
    snprintf(game_path, sizeof game_path, "%s", g->path);

    needInitCpu = true;
    ws_log("load: %s (HLE=%d)\n", g->path, Config.HLE);
    if (SysInit() < 0) return false;
    install_prof();
    ws_log("load: CheckCdrom\n");
    CheckCdrom();
    ws_log("load: SysReset\n");
    SysReset();
    ws_log("load: LoadCdrom\n");
    LoadCdrom();
    ws_log("load: done\n");
    /* memory cards */
    saveFile_init(saveFile_dir);
    LoadMcd(1, saveFile_dir);
    LoadMcd(2, saveFile_dir);
    game_loaded = true;
    return true;
}
void ps3_jit_selftest(void);
bool retro_load_game_special(unsigned t, const struct retro_game_info *i, size_t n) { return false; }
void retro_unload_game(void)
{
    if (!game_loaded) return;
    flush_cards();
    SysClose();
    game_loaded = false;
}
unsigned retro_get_region(void) { return Config.PsxType ? RETRO_REGION_PAL : RETRO_REGION_NTSC; }


/* ---- profiling (timebase ticks) ---- */
unsigned long long ws_prof[6];   /* 5 chd read, 0 gpu dma chain, 1 gpu write mem/data, 2 spu async, 3 execute */
static inline unsigned long long ws_tb(void) { unsigned long long t; __asm__ volatile("mftb %0" : "=r"(t)); return t; }
static long (*o_dmaChain)(unsigned long *, unsigned long, unsigned long *, unsigned long *);
static void (*o_writeDataMem)(unsigned long *, int);
static void (*o_writeData)(unsigned long);
static void (*o_spuasync)(uint32_t, uint32_t, uint32_t);
static long p_dmaChain(unsigned long *a, unsigned long b, unsigned long *c, unsigned long *d)
{ unsigned long long t = ws_tb(); long r = o_dmaChain(a, b, c, d); ws_prof[0] += ws_tb() - t; return r; }
static void p_writeDataMem(unsigned long *a, int n)
{ unsigned long long t = ws_tb(); o_writeDataMem(a, n); ws_prof[1] += ws_tb() - t; }
static void p_writeData(unsigned long v)
{ unsigned long long t = ws_tb(); o_writeData(v); ws_prof[1] += ws_tb() - t; }
static void p_spuasync(uint32_t a, uint32_t b, uint32_t c)
{ unsigned long long t = ws_tb(); o_spuasync(a, b, c); ws_prof[2] += ws_tb() - t; }
static void install_prof(void)
{
    if (SPU_async != p_spuasync) { o_spuasync = SPU_async; SPU_async = p_spuasync; }
    if (o_dmaChain) return;
    o_dmaChain = newSoftGpu.dmaChain; newSoftGpu.dmaChain = p_dmaChain;
    o_writeDataMem = newSoftGpu.writeDataMem; newSoftGpu.writeDataMem = p_writeDataMem;
    o_writeData = newSoftGpu.writeData; newSoftGpu.writeData = p_writeData;
}
/* returns ms per run for each bucket since last call, resets */
void ws_prof_str(char *b, int n, int runs)
{
    double k = 1000.0 / 79800000.0 / (runs ? runs : 1);
    extern unsigned long ws_dyn[4];
    snprintf(b, n, "prof ms/run: gpuchain=%.2f gpuwr=%.2f spu=%.2f (channels=%.2f) exec=%.2f | per run: flush=%.2f recompile=%.1f enters=%.1f",
             ws_prof[0] * k, ws_prof[1] * k, ws_prof[2] * k, ws_prof[4] * k, ws_prof[3] * k,
             (double)ws_dyn[0] / runs, (double)ws_dyn[1] / runs, (double)ws_dyn[2] / runs);
    { extern unsigned long long ws_spu_stat[16]; extern int ws_spu_render_on; size_t l = strlen(b);
      snprintf(b + l, n - l, " | spur=%d jobs/run=%.1f prims/run=%.0f syncwaits/run=%.1f waitms/run=%.2f maxspu_ms=%.2f full=%llu", ws_spu_render_on,
               (double)ws_spu_stat[0] / (runs ? runs : 1), (double)ws_spu_stat[1] / (runs ? runs : 1), (double)ws_spu_stat[2] / (runs ? runs : 1),
               ws_spu_stat[3] * k, ws_spu_stat[4] / 79800.0 * 1.0 / 1.0, ws_spu_stat[5]);
      l = strlen(b);
      snprintf(b + l, n - l, " | spuwork ms/run (summed over SPUs): total %.2f miss-wait %.2f (n %.0f) band-io %.2f draw %.2f (prim-bands %.0f) list-io %.2f",
               ws_spu_stat[6] / 79800.0 / (runs ? runs : 1), ws_spu_stat[7] / 79800.0 / (runs ? runs : 1), (double)ws_spu_stat[8] / (runs ? runs : 1),
               ws_spu_stat[9] / 79800.0 / (runs ? runs : 1), ws_spu_stat[11] / 79800.0 / (runs ? runs : 1), (double)ws_spu_stat[10] / (runs ? runs : 1), ws_spu_stat[12] / 79800.0 / (runs ? runs : 1));
      { extern unsigned long long ws_spu_kind[48]; static const char *kn[12] = {"F","G","FT4","FT8","FT15","GT4","GT8","GT15","SP4","SP8","SP15","TILE"}; int q;
        l = strlen(b); snprintf(b + l, n - l, " | kinds ms/run@us-per-primband:");
        for (q = 0; q < 12; q++) if (ws_spu_kind[12 + q]) { l = strlen(b); snprintf(b + l, n - l, " %s %.2f@%.1f[%.0fkpx %.0fkskip %.0fcyc/px]", kn[q], ws_spu_kind[q] / 79800.0 / (runs ? runs : 1), ws_spu_kind[q] / 79.8 / (double)ws_spu_kind[12 + q], ws_spu_kind[24 + q] / 1000.0 / (runs ? runs : 1), ws_spu_kind[36 + q] / 1000.0 / (runs ? runs : 1), ws_spu_kind[24 + q] ? ws_spu_kind[q] * 40.0 / (double)ws_spu_kind[24 + q] : 0.0); }
        { extern unsigned long long ws_spu_cls[16]; double r_ = runs ? runs : 1; l = strlen(b);
          snprintf(b + l, n - l, " | prims/run FT[plain-opaque %.0f modulated %.0f plain-semi %.0f mod-semi %.0f mask %.0f semi-abr0 %.0f semi-other %.0f interlaced %.0f] GT[plain-opaque %.0f modulated %.0f plain-semi %.0f mod-semi %.0f mask %.0f semi-abr0 %.0f semi-other %.0f interlaced %.0f]",
                   ws_spu_cls[0] / r_, ws_spu_cls[1] / r_, ws_spu_cls[2] / r_, ws_spu_cls[3] / r_, ws_spu_cls[4] / r_, ws_spu_cls[5] / r_, ws_spu_cls[6] / r_, ws_spu_cls[7] / r_,
                   ws_spu_cls[8] / r_, ws_spu_cls[9] / r_, ws_spu_cls[10] / r_, ws_spu_cls[11] / r_, ws_spu_cls[12] / r_, ws_spu_cls[13] / r_, ws_spu_cls[14] / r_, ws_spu_cls[15] / r_);
          memset(ws_spu_cls, 0, sizeof ws_spu_cls); }
        { extern unsigned long long ws_spu_ph[8]; l = strlen(b); snprintf(b + l, n - l, " | FT4quad phases ms/run (setup %.2f skip %.2f rowpro %.2f pixels %.2f rowend %.2f)", ws_spu_ph[0] / 79800.0 / (runs ? runs : 1), ws_spu_ph[1] / 79800.0 / (runs ? runs : 1), ws_spu_ph[2] / 79800.0 / (runs ? runs : 1), ws_spu_ph[3] / 79800.0 / (runs ? runs : 1), ws_spu_ph[4] / 79800.0 / (runs ? runs : 1)); memset(ws_spu_ph, 0, sizeof ws_spu_ph); }
        memset(ws_spu_kind, 0, sizeof ws_spu_kind); }
      memset(ws_spu_stat, 0, sizeof ws_spu_stat); }
    { extern unsigned long long ws_gst[12]; size_t l = strlen(b); double r = runs ? runs : 1;
      snprintf(b + l, n - l, " | cmd ms/run (n/run): poly %.2f (%.0f) spr %.2f (%.0f) line %.2f (%.0f) fill %.2f (%.0f) io %.2f (%.0f) syncwait %.2f (%.0f)",
               ws_gst[0] * k, ws_gst[6] / r, ws_gst[1] * k, ws_gst[7] / r, ws_gst[2] * k, ws_gst[8] / r, ws_gst[3] * k, ws_gst[9] / r, ws_gst[4] * k, ws_gst[10] / r, ws_gst[5] * k, ws_gst[11] / r);
      memset(ws_gst, 0, sizeof ws_gst); }
    memset(ws_dyn, 0, sizeof ws_dyn);
    memset(ws_prof, 0, sizeof ws_prof);
}

void retro_run(void)
{
    static unsigned frames;
    bool upd = false;
    if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &upd) && upd) { int a = pad_analog; update_variables(); (void)a; }
    input_poll_cb();
    frame_drawn = 0;
    stop = 0;
    { unsigned long long t0 = ws_tb(); psxCpu->Execute(); ws_prof[3] += ws_tb() - t0; }
    if (!frame_drawn) video_cb(NULL, out_w, out_h, out_w * 4);
    if (apos > 1) { audio_batch_cb(abuf, apos / 2); apos = 0; }
    {   /* memory cards: save ~2 s after the last write, and every 30 s as a safety net */
        static int idle; extern char mcd1Written, mcd2Written;
        if (mcd1Written || mcd2Written) {
            if (++idle >= 120 && saveFile_dir) {
                if (mcd1Written && mcd_queue(0)) mcd1Written = 0;
                if (mcd2Written && mcd_queue(1)) mcd2Written = 0;
                idle = 0;
            }
        } else idle = 0;
    }
    ++frames;
}

size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *d, size_t s) { return false; }
bool retro_unserialize(const void *d, size_t s) { return false; }
void *retro_get_memory_data(unsigned id) { return NULL; }
size_t retro_get_memory_size(unsigned id) { return 0; }
void retro_cheat_reset(void) { }
void retro_cheat_set(unsigned i, bool e, const char *c) { }

/* scheduler leftovers that only the Lightrec path used */
u32 event_cycles[PSXINT_COUNT];
u32 next_interupt;
void execI(void *r) { }
void irq_test(void *cp0) { }
u32 schedule_timeslice(void) { return 0; }
bool executingBios;
long CDR__about(void) { return 0; }
long CDR__configure(void) { return 0; }
long CDR__setfilename(char *f) { return 0; }
long CDR__test(void) { return 0; }
long GPU_init_stub(void) { return 0; }
long GPU_init(void) { return gpuPtr->init(); }
long GPU_shutdown(void) { return gpuPtr->shutdown(); }
gpu_t oldSoftGpu;

/* watchdog probe: called from another thread to see where the PSX CPU is */
extern volatile unsigned long ws_bc[8], ws_step, ws_iter, ws_exec;
void ws_probe(char *buf, int n)
{
    int o = snprintf(buf, n, "exec=%lu iter=%lu step=%lu bc: cmd=%lx list=%lx end=%lx len=%lu ncmds=%lu done=%lx | chain addr=%lx count=%lu | ",
        ws_exec, ws_iter, ws_step, ws_bc[0], ws_bc[1], ws_bc[2], ws_bc[3], ws_bc[4], ws_bc[5], ws_bc[6], ws_bc[7]);
    buf += o; n -= o;
    snprintf(buf, n, "pc=%08x cycle=%u intr=%08x stop=%d next_int=%u ra=%08x sp=%08x rec=%d code=%08x ram1000=%08x %08x %08x %08x %08x HLE=%d",
             (unsigned)psxRegs.pc, (unsigned)psxRegs.cycle, (unsigned)psxRegs.interrupt, (int)stop,
             (unsigned)next_interupt, (unsigned)psxRegs.GPR.n.ra, (unsigned)psxRegs.GPR.n.sp, (int)psxRegs.cpuInRecursion,
             (unsigned)psxRegs.code,
             SWAP32(*(u32 *)(psxM + 0x1000)), SWAP32(*(u32 *)(psxM + 0x1004)), SWAP32(*(u32 *)(psxM + 0x1008)),
             SWAP32(*(u32 *)(psxM + 0x100c)), SWAP32(*(u32 *)(psxM + 0x1010)), (int)Config.HLE);
}
extern volatile unsigned long ws_ring[16]; extern volatile unsigned ws_ring_i;
void ws_probe2(char *buf, int n)
{
    extern volatile int ws_where, ws_hcinfo[4]; extern volatile int hc_dbg_meta, hc_dbg_dec;
    { int o0 = snprintf(buf, n, "where=%d hc: wait=%d main=%d pf=%d | ", ws_where, ws_hcinfo[0], ws_hcinfo[1], ws_hcinfo[2]); buf += o0; n -= o0; }
    unsigned i, k = ws_ring_i; int o = snprintf(buf, n, "last fns:");
    for (i = 0; i < 12 && o < n - 12; i++) o += snprintf(buf + o, n - o, " %lx", ws_ring[(k - 1 - i) & 15]);
}

/* frontend asks the GPU to skip drawing the next frame when running late */
void ws_set_skip(int on) { pl_rearmed_cbs.fskip_advice = on; }

/* ---- PSX-pc sampling histogram (called from the watchdog thread) ---- */
static unsigned short ws_hist[8192 + 2048];
static unsigned ws_hist_n;
void ws_hist_add(void)
{
    unsigned pc = (unsigned)psxRegs.pc, k;
    if ((pc & 0x1fc00000) == 0x1fc00000) k = 8192 + ((pc & 0x7ffff) >> 8);
    else k = (pc & 0x1fffff) >> 8;
    if (ws_hist[k] != 0xffff) ws_hist[k]++;
    ws_hist_n++;
}
void ws_hist_report(char *b, int n)
{
    int i, j, top[8] = {0}; int o;
    for (j = 0; j < 8; j++) {
        int best = -1; unsigned bc = 0;
        for (i = 0; i < 8192 + 2048; i++) {
            int used = 0, q;
            for (q = 0; q < j; q++) if (top[q] == i) used = 1;
            if (!used && ws_hist[i] > bc) { bc = ws_hist[i]; best = i; }
        }
        top[j] = best < 0 ? 0 : best;
    }
    o = snprintf(b, n, "hist(%u):", ws_hist_n);
    for (j = 0; j < 8 && o < n - 24; j++) {
        int k = top[j];
        if (!ws_hist[k]) break;
        o += snprintf(b + o, n - o, " %s%05x=%u", k >= 8192 ? "B" : "R", (k >= 8192 ? (k - 8192) : k) << 8, ws_hist[k]);
    }
    memset(ws_hist, 0, sizeof ws_hist); ws_hist_n = 0;
}
