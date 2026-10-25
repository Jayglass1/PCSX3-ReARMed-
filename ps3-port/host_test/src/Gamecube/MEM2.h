/* PS3 replacement for the Wii MEM2 map: plain static buffers. */
#ifndef MEM2_H
#define MEM2_H
#define KB (1024)
#define MB (1024*1024)
extern char ps3_mcd1_buf[128*1024];
extern char ps3_mcd2_buf[128*1024];
extern char ps3_spu_buf[512*1024];
extern char ps3_rec_data[4*1024*1024];
extern char ps3_jit_buffer[];          /* executable code cache (in .text) */
#define MCD1_SIZE     (128*KB)
#define MCD1_LO       ((char*)ps3_mcd1_buf)
#define MCD2_SIZE     (128*KB)
#define MCD2_LO       ((char*)ps3_mcd2_buf)
#define RECMEM2_SIZE  (8*MB)
#define RECMEM2_LO    ((char*)ps3_jit_buffer)
#define RECMEM2_HI    (RECMEM2_LO + RECMEM2_SIZE)
#define SPU_BUF_SIZE  (512*KB)
#define SPU_BUF_LO    ((char*)ps3_spu_buf)
#define LIGHTREC_BUF_SIZE (4*MB)
#define LIGHTREC_BUF_LO   ((char*)ps3_rec_data)
#define LIGHTREC_BUF_HI   (LIGHTREC_BUF_LO + LIGHTREC_BUF_SIZE)
#endif
