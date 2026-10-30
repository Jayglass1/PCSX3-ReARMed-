#include <stdint.h>
#include "lightrec.h"
extern void out_s(const char*); extern void out_x(unsigned long); extern void out_d(long); extern void out_c(char);
#define EXITR (*(volatile unsigned int *)0xF0000010UL)
static uint8_t ram[0x200000] __attribute__((aligned(4096)));
static uint8_t bios[0x80000], spad[0x400], pport[0x10000], hwr[0x8000];
static uint8_t codebuf[0x400000] __attribute__((aligned(4096)));
extern long ws_mprotect(void*,unsigned long,int); extern void ws_exit(int);
static int hwn;
static u8 hw_lb(struct lightrec_state *s,u32 o,void*h,u32 a){return 0;}
static u16 hw_lh(struct lightrec_state *s,u32 o,void*h,u32 a){return 0;}
static u32 hw_lw(struct lightrec_state *s,u32 o,void*h,u32 a){return 0;}
static void hw_sb(struct lightrec_state *s,u32 o,void*h,u32 a,u32 d){hwn++;}
static void hw_sh(struct lightrec_state *s,u32 o,void*h,u32 a,u32 d){hwn++;}
static void hw_sw(struct lightrec_state *s,u32 o,void*h,u32 a,u32 d){hwn++;}
static struct lightrec_mem_map_ops hw_ops={hw_sb,hw_sh,hw_sw,hw_lb,hw_lh,hw_lw,hw_lw,hw_sw};
static void cc_w(struct lightrec_state *s,u32 o,void*h,u32 a,u32 d){}
static u32 cc_r(struct lightrec_state *s,u32 o,void*h,u32 a){return 0;}
static struct lightrec_mem_map_ops cc_ops={.sw=cc_w,.lw=cc_r};
static struct lightrec_mem_map map[]={
 [PSX_MAP_KERNEL_USER_RAM]={.pc=0,.length=0x200000},
 [PSX_MAP_BIOS]={.pc=0x1fc00000,.length=0x80000},
 [PSX_MAP_SCRATCH_PAD]={.pc=0x1f800000,.length=0x400},
 [PSX_MAP_PARALLEL_PORT]={.pc=0x1f000000,.length=0x10000},
 [PSX_MAP_HW_REGISTERS]={.pc=0x1f801000,.length=0x8000,.ops=&hw_ops},
 [PSX_MAP_CACHE_CONTROL]={.pc=0x5ffe0130,.length=4,.ops=&cc_ops},
 [PSX_MAP_MIRROR1]={.pc=0x200000,.length=0x200000,.mirror_of=&map[PSX_MAP_KERNEL_USER_RAM]},
 [PSX_MAP_MIRROR2]={.pc=0x400000,.length=0x200000,.mirror_of=&map[PSX_MAP_KERNEL_USER_RAM]},
 [PSX_MAP_MIRROR3]={.pc=0x600000,.length=0x200000,.mirror_of=&map[PSX_MAP_KERNEL_USER_RAM]},
 [PSX_MAP_PPORT_MIRROR]={.pc=0x1fa00000,.length=0x10000,.mirror_of=&map[PSX_MAP_PARALLEL_PORT]},
 [PSX_MAP_CODE_BUFFER]={.length=sizeof codebuf},
};
static void cop2_op(struct lightrec_state *s,u32 op){}
static void enable_ram(struct lightrec_state *s,_Bool e){}
static _Bool hw_direct(u32 a,_Bool w,u8 sz){return 0;}
static void code_inv(void*p,u32 l){}
static const struct lightrec_ops ops={.cop2_op=cop2_op,.enable_ram=enable_ram,.hw_direct=hw_direct,.code_inv=code_inv};

#define I(op,rs,rt,imm) (((op)<<26)|((rs)<<21)|((rt)<<16)|((imm)&0xffff))
#define R(rs,rt,rd,sh,f) (((rs)<<21)|((rt)<<16)|((rd)<<11)|((sh)<<6)|(f))
#define J(a) ((2u<<26)|(((a)>>2)&0x3ffffff))
static void put(uint32_t off, uint32_t w){ /* little-endian raw BIOS */
 bios[off]=w; bios[off+1]=w>>8; bios[off+2]=w>>16; bios[off+3]=w>>24; }
void run_test(void)
{
 uint32_t o=0, i;
 /* block A: 4 hw writes, jump */
 put(o,I(0x0f,0,8,0x1f80)); o+=4;          /* lui t0,0x1f80 */
 put(o,I(0x09,0,9,0x1234)); o+=4;
 for(i=0;i<4;i++){ put(o,I(0x2b,8,9,0x1010+i*4)); o+=4; }
 put(o,J(0xbfc00040)); o+=4; put(o,0); o+=4;
 o=0x40;
 /* block B: t2=0,t3=0xf80 ; loop: 8 stores; t2+=0x20; bne t2,t3,loop ; then store marker, jump to C */
 put(o,I(0x09,0,10,0)); o+=4; put(o,I(0x09,0,11,0xf80)); o+=4; put(o,0); o+=4;
 uint32_t loop=o;
 for(i=0;i<8;i++){ put(o,I(0x2b,10,0,i*4)); o+=4; }
 put(o,I(0x09,10,10,0x20)); o+=4;
 put(o,I(0x05,10,11,((int)(loop-(o+4))>>2)&0xffff)); o+=4; put(o,0); o+=4; /* bne t2,t3,loop; nop */
 put(o,I(0x09,0,12,0x7777)); o+=4;
 put(o,I(0x2b,0,12,0x100)); o+=4;
 put(o,J(0xbfc00100)); o+=4; put(o,0); o+=4;
 /* C: spin */
 put(0x100,I(0x09,0,13,0x55)); put(0x104,J(0xbfc00100)); put(0x108,0);
 struct lightrec_state *st;
 for(i=0;i<sizeof ram;i++) ram[i]=0xAA;
 map[PSX_MAP_KERNEL_USER_RAM].address=ram; map[PSX_MAP_BIOS].address=bios; map[PSX_MAP_SCRATCH_PAD].address=spad;
 map[PSX_MAP_PARALLEL_PORT].address=pport; map[PSX_MAP_HW_REGISTERS].address=hwr; map[PSX_MAP_CODE_BUFFER].address=codebuf;
 out_s("init\n");
 st=lightrec_init("t",map,sizeof(map)/sizeof(map[0]),&ops);
 if(!st){ out_s("init failed\n"); return; }
 out_s("init ok\n");
 struct lightrec_registers *r=lightrec_get_registers(st);
 uint32_t pc=0xbfc00000; 
 for(i=0;i<40;i++){
  out_s("exec "); out_d(i); out_s(" pc="); out_x(pc);
  pc=lightrec_execute(st,pc,0);
  out_s(" -> "); out_x(pc); out_s(" flags="); out_x(lightrec_exit_flags(st)); out_c('\n');
  if(pc==0xbfc00100 && r->gpr[13]==0x55) break;
 }
 out_s("hwn="); out_d(hwn); out_s(" ram0="); out_x(ram[0]|ram[1]<<8|ram[2]<<16|ram[3]<<24); out_s(" ramf7c="); out_x(ram[0xf7c]|ram[0xf7d]<<8);
 out_s(" ram100="); out_x(ram[0x100]|ram[0x101]<<8); out_s(" ramf80="); out_x(ram[0xf80]); out_c('\n');
 out_s(r->gpr[13]==0x55?"TEST PASS\n":"TEST FAIL\n");
}
void _start(void){ ws_mprotect(codebuf,sizeof codebuf,7); run_test(); ws_exit(0); }
