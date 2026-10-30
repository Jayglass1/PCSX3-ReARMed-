/* Host test: Lightrec JIT (GNU Lightning backend) vs Lightrec's own interpreter
 * on random MIPS programs.  Build: see host_test/build_lr_test.sh */
#include <stddef.h>
extern char *getenv(const char*); extern int atoi(const char*); extern void exit(int); extern void perror(const char*); extern int printf(const char*,...); extern int fprintf(void*,const char*,...); extern void *stderr; extern int memcmp(const void*,const void*,unsigned long); extern void *memset(void*,int,unsigned long); extern void *memcpy(void*,const void*,unsigned long);
#include <stdint.h>
#include "lightrec.h"

static uint8_t ram[0x200000] __attribute__((aligned(4096)));
static uint8_t bios[0x80000], spad[0x400], pport[0x10000], hwr[0x8000];
static uint8_t codebuf[0x400000] __attribute__((aligned(4096)));
#define CODE_SZ 0x400000

static u8 hw_lb(struct lightrec_state *s, u32 o, void *h, u32 a){ return 0; }
static u16 hw_lh(struct lightrec_state *s, u32 o, void *h, u32 a){ return 0; }
static u32 hw_lw(struct lightrec_state *s, u32 o, void *h, u32 a){ return 0; }
static void hw_sb(struct lightrec_state *s, u32 o, void *h, u32 a, u32 d){}
static void hw_sh(struct lightrec_state *s, u32 o, void *h, u32 a, u32 d){}
static void hw_sw(struct lightrec_state *s, u32 o, void *h, u32 a, u32 d){}
static struct lightrec_mem_map_ops hw_ops = { hw_sb, hw_sh, hw_sw, hw_lb, hw_lh, hw_lw, hw_lw, hw_sw };
static void cc_w(struct lightrec_state *s, u32 o, void *h, u32 a, u32 d){}
static u32 cc_r(struct lightrec_state *s, u32 o, void *h, u32 a){ return 0; }
static struct lightrec_mem_map_ops cc_ops = { .sw = cc_w, .lw = cc_r };

static struct lightrec_mem_map map[] = {
	[PSX_MAP_KERNEL_USER_RAM] = { .pc = 0, .length = 0x200000 },
	[PSX_MAP_BIOS] = { .pc = 0x1fc00000, .length = 0x80000 },
	[PSX_MAP_SCRATCH_PAD] = { .pc = 0x1f800000, .length = 0x400 },
	[PSX_MAP_PARALLEL_PORT] = { .pc = 0x1f000000, .length = 0x10000 },
	[PSX_MAP_HW_REGISTERS] = { .pc = 0x1f801000, .length = 0x8000, .ops = &hw_ops },
	[PSX_MAP_CACHE_CONTROL] = { .pc = 0x5ffe0130, .length = 4, .ops = &cc_ops },
	[PSX_MAP_MIRROR1] = { .pc = 0x200000, .length = 0x200000, .mirror_of = &map[PSX_MAP_KERNEL_USER_RAM] },
	[PSX_MAP_MIRROR2] = { .pc = 0x400000, .length = 0x200000, .mirror_of = &map[PSX_MAP_KERNEL_USER_RAM] },
	[PSX_MAP_MIRROR3] = { .pc = 0x600000, .length = 0x200000, .mirror_of = &map[PSX_MAP_KERNEL_USER_RAM] },
	[PSX_MAP_PPORT_MIRROR] = { .pc = 0x1fa00000, .length = 0x10000, .mirror_of = &map[PSX_MAP_PARALLEL_PORT] },
	[PSX_MAP_CODE_BUFFER] = { .length = CODE_SZ },
};

static void cop2_op(struct lightrec_state *s, u32 op) {}
static void enable_ram(struct lightrec_state *s, _Bool e) {}
static _Bool hw_direct(u32 a, _Bool w, u8 sz) { return 0; }
static void code_inv(void *p, u32 l) {}
static const struct lightrec_ops ops = { .cop2_op = cop2_op, .enable_ram = enable_ram, .hw_direct = hw_direct, .code_inv = code_inv };

static uint64_t rs;
static uint32_t rnd(void){ rs = rs * 6364136223846793005ULL + 1442695040888963407ULL; return rs >> 33; }

#define PROG 0x10000
#define DATA 0x20000
static int gen(uint32_t *p, int n)
{
	int i, k = 0;
	for (i = 0; i < n; i++) {
		uint32_t rsr = 1 + rnd() % 15, rtr = 1 + rnd() % 15, rdr = 1 + rnd() % 15, sh = rnd() % 32, imm = rnd() & 0xffff;
		uint32_t op;
		int cls = getenv("CLS") ? atoi(getenv("CLS")) : 15; int sel = rnd() % 24;
		if (sel < 19 && !(cls & 1)) sel = 19 + (cls & 2 ? 0 : 4);
		if (sel == 19 && !(cls & 2)) sel = 20 + 0;
		if (sel >= 20 && !(cls & 4)) sel = (cls & 1) ? 0 : 19;
		switch (sel) {
		case 0: op = (rsr<<21)|(rtr<<16)|(rdr<<11)|0x21; break; /* addu */
		case 1: op = (rsr<<21)|(rtr<<16)|(rdr<<11)|0x23; break; /* subu */
		case 2: op = (rsr<<21)|(rtr<<16)|(rdr<<11)|0x24; break; /* and */
		case 3: op = (rsr<<21)|(rtr<<16)|(rdr<<11)|0x25; break; /* or */
		case 4: op = (rsr<<21)|(rtr<<16)|(rdr<<11)|0x26; break; /* xor */
		case 5: op = (rsr<<21)|(rtr<<16)|(rdr<<11)|0x27; break; /* nor */
		case 6: op = (rsr<<21)|(rtr<<16)|(rdr<<11)|0x2a; break; /* slt */
		case 7: op = (rsr<<21)|(rtr<<16)|(rdr<<11)|0x2b; break; /* sltu */
		case 8: op = (rtr<<16)|(rdr<<11)|(sh<<6)|0x00; break; /* sll */
		case 9: op = (rtr<<16)|(rdr<<11)|(sh<<6)|0x02; break; /* srl */
		case 10: op = (rtr<<16)|(rdr<<11)|(sh<<6)|0x03; break; /* sra */
		case 11: op = (rsr<<21)|(rtr<<16)|(rdr<<11)|0x04; break; /* sllv */
		case 12: op = (rsr<<21)|(rtr<<16)|(rdr<<11)|0x06; break; /* srlv */
		case 13: op = (rsr<<21)|(rtr<<16)|(rdr<<11)|0x07; break; /* srav */
		case 14: op = (0x09<<26)|(rsr<<21)|(rtr<<16)|imm; break; /* addiu */
		case 15: op = (0x0c<<26)|(rsr<<21)|(rtr<<16)|imm; break; /* andi */
		case 16: op = (0x0d<<26)|(rsr<<21)|(rtr<<16)|imm; break; /* ori */
		case 17: op = (0x0f<<26)|(rtr<<16)|imm; break; /* lui */
		case 18: op = (0x0a<<26)|(rsr<<21)|(rtr<<16)|imm; break; /* slti */
		case 19: { /* mult/multu/div/divu then mflo/mfhi */
			static const int f[] = {0x18,0x19,0x1a,0x1b};
			p[k++] = (rsr<<21)|(rtr<<16)|f[rnd()%4];
			op = (rdr<<11)|((rnd()&1)?0x10:0x12); break; }
		default: { /* load/store relative to r28 (data base) */
			static const int o[] = {0x20,0x21,0x23,0x24,0x25,0x28,0x29,0x2b,0x22,0x26,0x2a,0x2e};
			uint32_t off = rnd() % 0x3f0;
			uint32_t c = o[rnd() % 12];
			if (c==0x21||c==0x25||c==0x29) off &= ~1u;
			if (c==0x23||c==0x2b) off &= ~3u;
			op = (c<<26)|(28<<21)|(rtr<<16)|off; break; }
		}
		p[k++] = op;
		if ((cls & 8) && rnd() % 17 == 0) { /* forward branch over 1..3 insns incl. delay slot */
			uint32_t t = 1 + rnd() % 3;
			p[k++] = ((rnd()&1?0x04:0x05)<<26)|(rsr<<21)|(rtr<<16)|t;
			p[k++] = (0x09<<26)|(rdr<<21)|(rdr<<16)|(rnd()&0xff); /* delay slot */
			for (uint32_t j = 0; j < t; j++) p[k++] = (0x0d<<26)|(rdr<<21)|(rdr<<16)|(rnd()&0xff);
		}
	}
	p[k] = (2u << 26) | (((PROG + (k + 2) * 4) >> 2) & 0x3ffffff); k++; /* j to the break below (ends the block with a jump, like real code) */
	p[k++] = 0; /* delay slot nop */
	p[k++] = 0x0000000d; /* break */
	p[k] = (2u << 26) | (((PROG + k * 4) >> 2) & 0x3ffffff); k++; /* block terminator after the break (Lightrec only ends blocks at jumps/syscall) */
	p[k++] = 0;
	return k;
}


/* independent reference interpreter (ALU, mul/div, branches, loads/stores to the data area) */
static void ref_run(const uint32_t *prog, int n, const uint32_t *gi, uint32_t *g, uint8_t *mem)
{
	uint32_t r[32], hi = 0, lo = 0; int pc = 0, steps = 0;
	memcpy(r, gi, sizeof r);
	memset(ram, 0, sizeof(ram));
	for (int q = 0; q < n; q++) { uint32_t w = prog[q]; ram[PROG+q*4] = w; ram[PROG+q*4+1] = w >> 8; ram[PROG+q*4+2] = w >> 16; ram[PROG+q*4+3] = w >> 24; }
	{ uint64_t t = rs; for (int i = 0; i < 0x400; i++) { rs = rs*6364136223846793005ULL+1; ram[DATA+i] = rs >> 40; } rs = t; }
	while (pc < n && steps++ < 100000) {
		uint32_t x = prog[pc], op = x >> 26, rs_ = (x >> 21) & 31, rt = (x >> 16) & 31, rd = (x >> 11) & 31, sh = (x >> 6) & 31, f = x & 63;
		int16_t si = x; uint32_t ui = x & 0xffff, a, v; int taken = 0, br = 0;
		if (x == 0xd || op == 2) break;
		if (op == 0) switch (f) {
		case 0x00: r[rd] = r[rt] << sh; break; case 0x02: r[rd] = r[rt] >> sh; break; case 0x03: r[rd] = (int32_t)r[rt] >> sh; break;
		case 0x04: r[rd] = r[rt] << (r[rs_] & 31); break; case 0x06: r[rd] = r[rt] >> (r[rs_] & 31); break; case 0x07: r[rd] = (int32_t)r[rt] >> (r[rs_] & 31); break;
		case 0x10: r[rd] = hi; break; case 0x12: r[rd] = lo; break;
		case 0x18: { int64_t m = (int64_t)(int32_t)r[rs_] * (int32_t)r[rt]; lo = m; hi = m >> 32; } break;
		case 0x19: { uint64_t m = (uint64_t)r[rs_] * r[rt]; lo = m; hi = m >> 32; } break;
		case 0x1a: if (!r[rt]) { lo = (int32_t)r[rs_] < 0 ? 1 : -1; hi = r[rs_]; } else if (r[rs_] == 0x80000000 && r[rt] == 0xffffffff) { lo = 0x80000000; hi = 0; } else { lo = (int32_t)r[rs_] / (int32_t)r[rt]; hi = (int32_t)r[rs_] % (int32_t)r[rt]; } break;
		case 0x1b: if (!r[rt]) { lo = -1; hi = r[rs_]; } else { lo = r[rs_] / r[rt]; hi = r[rs_] % r[rt]; } break;
		case 0x21: r[rd] = r[rs_] + r[rt]; break; case 0x23: r[rd] = r[rs_] - r[rt]; break; case 0x24: r[rd] = r[rs_] & r[rt]; break;
		case 0x25: r[rd] = r[rs_] | r[rt]; break; case 0x26: r[rd] = r[rs_] ^ r[rt]; break; case 0x27: r[rd] = ~(r[rs_] | r[rt]); break;
		case 0x2a: r[rd] = (int32_t)r[rs_] < (int32_t)r[rt]; break; case 0x2b: r[rd] = r[rs_] < r[rt]; break;
		}
		else switch (op) {
		case 0x04: br = 1; taken = r[rs_] == r[rt]; break; case 0x05: br = 1; taken = r[rs_] != r[rt]; break;
		case 0x09: r[rt] = r[rs_] + si; break; case 0x0a: r[rt] = (int32_t)r[rs_] < (int32_t)si; break;
		case 0x0c: r[rt] = r[rs_] & ui; break; case 0x0d: r[rt] = r[rs_] | ui; break; case 0x0f: r[rt] = ui << 16; break;
		default:
			a = (r[rs_] + si) - 0x80000000u; 
			if (a >= 0x20000 + 0x3ff || a < 0x20000) break;
			a -= 0; { uint8_t *m = ram + a; uint32_t al = a & 3;
			switch (op) {
			case 0x20: r[rt] = (int8_t)*m; break; case 0x24: r[rt] = *m; break;
			case 0x21: r[rt] = (int16_t)(m[0] | m[1] << 8); break; case 0x25: r[rt] = m[0] | m[1] << 8; break;
			case 0x23: r[rt] = m[0] | m[1] << 8 | m[2] << 16 | (uint32_t)m[3] << 24; break;
			case 0x28: *m = r[rt]; break; case 0x29: m[0] = r[rt]; m[1] = r[rt] >> 8; break;
			case 0x2b: m[0] = r[rt]; m[1] = r[rt] >> 8; m[2] = r[rt] >> 16; m[3] = r[rt] >> 24; break;
			case 0x22: { uint8_t *b = m - al; v = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; r[rt] = (r[rt] & (0x00ffffffu >> (al * 8))) | (v << (24 - al * 8)); } break; /* lwl */
			case 0x26: { uint8_t *b = m - al; v = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; r[rt] = (al == 0 ? v : (r[rt] & (0xffffff00u << ((3 - al) * 8))) | (v >> (al * 8))); } break; /* lwr */
			case 0x2a: { uint8_t *b = m - al; v = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; uint32_t nv = (v & (0xffffff00u << (al * 8))) | (r[rt] >> (24 - al * 8)); b[0]=nv; b[1]=nv>>8; b[2]=nv>>16; b[3]=nv>>24; } break; /* swl */
			case 0x2e: { uint8_t *b = m - al; v = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; uint32_t nv = al == 0 ? r[rt] : (v & (0x00ffffffu >> ((3 - al) * 8))) | (r[rt] << (al * 8)); b[0]=nv; b[1]=nv>>8; b[2]=nv>>16; b[3]=nv>>24; } break; /* swr */
			} }
		}
		r[0] = 0;
		if (br) { /* delay slot executes unconditionally: ALU only in generator */
			uint32_t d = prog[pc + 1], dop = d >> 26, drt = (d >> 16) & 31, drs = (d >> 21) & 31; (void)dop;
			r[drt] = r[drs] + (int16_t)d;
			pc += 2 + (taken ? (int16_t)x : 0);
			continue;
		}
		pc++;
	}
	memcpy(g, r, 32 * 4); g[32] = hi; g[33] = lo;
	memcpy(mem, ram + DATA, 0x400);
}

static int run(int jit, uint32_t *prog, int n, uint32_t *gpr_init, uint32_t *gpr_out, uint8_t *mem_out)
{
	struct lightrec_state *st;
	struct lightrec_registers *r;
	memset(ram, 0, sizeof(ram));
	for (int q = 0; q < n; q++) { uint32_t w = prog[q]; ram[PROG+q*4] = w; ram[PROG+q*4+1] = w >> 8; ram[PROG+q*4+2] = w >> 16; ram[PROG+q*4+3] = w >> 24; }
	/* deterministic data area */
	{ uint64_t t = rs; for (int i = 0; i < 0x400; i++) { rs = rs*6364136223846793005ULL+1; ram[DATA+i] = rs >> 40; } rs = t; }
	memset(codebuf, 0, CODE_SZ);
	map[PSX_MAP_KERNEL_USER_RAM].address = ram;
	map[PSX_MAP_BIOS].address = bios;
	map[PSX_MAP_SCRATCH_PAD].address = spad;
	map[PSX_MAP_PARALLEL_PORT].address = pport;
	map[PSX_MAP_HW_REGISTERS].address = hwr;
	map[PSX_MAP_CODE_BUFFER].address = codebuf;
	st = lightrec_init("lr_test", map, sizeof(map)/sizeof(map[0]), &ops);
	if (!st) { fprintf(stderr, "init failed\n"); exit(2); }
	r = lightrec_get_registers(st);
	for (int pass = 0; pass < (jit ? 4 : 1); pass++) {
		/* FIRST_PASS interprets a block the first times it runs; repeat so the JIT code is what is measured */
		{ uint64_t t = rs; for (int i = 0; i < 0x400; i++) { rs = rs*6364136223846793005ULL+1; ram[DATA+i] = rs >> 40; } rs = t; }
		memset(r, 0, sizeof(*r));
		memcpy(r->gpr, gpr_init, 32 * 4);
		r->cp0[12] = 0x10900000; r->cp0[15] = 2;
		if (jit) lightrec_execute(st, 0x80000000 + PROG, 1000000 * 1024);
		else lightrec_run_interpreter(st, 0x80000000 + PROG, 1000000 * 1024);
	}
	memcpy(gpr_out, r->gpr, 34 * 4);
	memcpy(mem_out, ram + DATA, 0x400);
	int fl = lightrec_exit_flags(st);
	lightrec_destroy(st);
	return fl;
}

int lrmain(int argc, char **argv)
{
	int cref0 = 0, cref1 = 0; int iters = argc > 1 ? atoi(argv[1]) : 500, bad = 0, only = argc > 2 ? atoi(argv[2]) : -1;
	for (int it = (only >= 0 ? only : 0); it < (only >= 0 ? only + 1 : iters); it++) {
		uint32_t prog[2048], gi[32], g0[34], g1[34], gr[34]; static uint8_t mr[0x400]; static uint8_t m0[0x400], m1[0x400];
		rs = 1000 + it;
		memset(gi, 0, sizeof gi);
		for (int i = 1; i < 32; i++) gi[i] = rnd() ^ (rnd() << 16);
		gi[28] = 0x80000000 + DATA; /* base for loads/stores */
		int n = gen(prog, getenv("LEN") ? atoi(getenv("LEN")) : 150 + rnd() % 200);
		uint64_t keep = rs;
		ref_run(prog, n, gi, gr, mr); rs = keep;
		int f0 = run(0, prog, n, gi, g0, m0); rs = keep;
		int f1 = run(1, prog, n, gi, g1, m1);
		int dref = memcmp(g0, gr, 32 * 4) || memcmp(m0, mr, 0x400), dref1 = memcmp(g1, gr, 32 * 4) || memcmp(m1, mr, 0x400); cref0 += dref; cref1 += dref1; if (dref && it < 3) { for (int i = 0; i < 32; i++) if (g0[i] != gr[i]) { printf("it%d r%d int=%08x ref=%08x\n", it, i, g0[i], gr[i]); break; } for (int i = 0; i < 0x400; i++) if (m0[i] != mr[i]) { printf("it%d mem+%x int=%02x ref=%02x\n", it, i, m0[i], mr[i]); break; } }
		int diff = (f0 != f1) || memcmp(g0, g1, 32 * 4) || memcmp(m0, m1, 0x400);
		if (diff) {
			bad++;
			if (bad < 6) {
				printf("MISMATCH iter %d flags %x/%x\n", it, f0, f1); if (only >= 0) { for (int i = 0; i < 32; i++) if (gr[i] != g0[i] || gr[i] != g1[i]) printf("  REF r%d=%08x\n", i, gr[i]); for (int i = 0; i < n; i++) printf("%05x: %08x\n", PROG + i*4, prog[i]); for (int i = 0; i < 32; i++) printf("init r%d=%08x\n", i, gi[i]); }
				for (int i = 0; i < 32; i++) if (g0[i] != g1[i] || only >= 0) printf("  r%d int=%08x jit=%08x\n", i, g0[i], g1[i]);
				for (int i = 0; i < 0x400; i++) if (m0[i] != m1[i]) { printf("  mem+%x int=%02x jit=%02x\n", i, m0[i], m1[i]); break; }
			}
		}
	}
	printf("lr_test: %d iterations, %d mismatches (vs ref: interp %d, jit %d)\n", iters, bad, cref0, cref1);
	return bad != 0;
}

extern void ws_exit(int);
void _start(void){ char *a[3]={"x","200",0}; int r=lrmain(2,a); ws_exit(r); }
