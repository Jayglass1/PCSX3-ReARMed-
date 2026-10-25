/* PS3: JIT code buffer in a read/write/execute segment of the image,
 * since the heap is not executable. */


/* lv2 only executes the loaded image's text pages (and allows writing them
 * at run time despite the R-E segment flags), so the code cache is a .text
 * array. Nothing in an RW segment is executable. (Found by NotDC-PS3.) */
unsigned char ps3_jit_buffer[0x800000]
	__attribute__((aligned(4096), section(".text"), used));

#include <stdio.h>
#include <stdarg.h>
/* Diagnostic log, flushed every line so the last line survives a crash. */
void ps3_jit_log(const char *fmt, ...)
{
	static FILE *f; static int n;
	va_list ap;
	if (!f) { f = fopen("/dev_hdd0/tmp/wiistation_jit.log", "w"); if (!f) return; }
	if (n++ > 4000) return;
	va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
	fflush(f);
}

void ps3_jit_dump(const void *p, unsigned n)
{
	static int done;
	FILE *f;
	if (done) return;
	done = 1;
	f = fopen("/dev_hdd0/tmp/wiistation_jit.bin", "wb");
	if (!f) return;
	fwrite(p, 1, n, f);
	fclose(f);
}

void ps3_jit_hex(const char *tag, const void *code, unsigned size)
{
	static int calls;
	const unsigned char *c = code;
	unsigned i;
	if (calls++ >= 6) return;
	if (size > 2048) size = 2048;
	ps3_jit_log("DUMP %s @%p size=%u\n", tag, code, size);
	for (i = 0; i < size; i += 16) {
		unsigned j; char buf[80]; char *o = buf;
		for (j = 0; j < 16 && i + j < size; j++)
			o += sprintf(o, "%02x", c[i + j]);
		ps3_jit_log("%04x: %s\n", i, buf);
	}
}

/* Self-test: run a tiny stub from the code cache and log it. */
static void run_stub(const char *name, void *mem)
{
	volatile unsigned int *p = (volatile unsigned int *)mem;
	static unsigned long desc[3];
	int (*f)(void);
	unsigned long a;
	p[0] = 0x3860002a; /* li r3,42 */
	p[1] = 0x4e800020; /* blr */
	for (a = (unsigned long)p & ~127UL; a < (unsigned long)(p + 2); a += 128)
		__asm__ volatile ("dcbst 0,%0" : : "r"(a) : "memory");
	__asm__ volatile ("sync" : : : "memory");
	for (a = (unsigned long)p & ~127UL; a < (unsigned long)(p + 2); a += 128)
		__asm__ volatile ("icbi 0,%0" : : "r"(a) : "memory");
	__asm__ volatile ("isync" : : : "memory");
	{ unsigned long r2v; __asm__ volatile("mr %0,2" : "=r"(r2v)); desc[0] = (unsigned long)p; desc[1] = r2v; desc[2] = 0; __asm__ volatile("" : : "r"(desc) : "memory"); }
	ps3_jit_log("selftest [%s]: stub at %p, calling\n", name, mem);
	f = (int (*)(void))(void *)desc;
	{ int r = f(); ps3_jit_log("selftest [%s]: returned %d => EXECUTABLE\n", name, r); }
}

void ps3_jit_selftest(void)
{
	ps3_jit_log("WiiStation core build v27 (" __DATE__ " " __TIME__ ")\n");
	run_stub("A: start of .text cache", ps3_jit_buffer);
	run_stub("B: end of .text cache", ps3_jit_buffer + 0x7ff000);
}

/* Staged diagnostics: each call runs one stage; the app records the stage
 * before running it, so after a crash the next launch moves on. */
static int __attribute__((noinline, used)) ctrl_fn(void) { return 7; }
static int __attribute__((noinline, used)) patch_fn(void) { return 7; }

static void sync_range(volatile unsigned int *p, int n)
{
	unsigned long a;
	for (a = (unsigned long)p & ~127UL; a < (unsigned long)(p + n); a += 128)
		__asm__ volatile ("dcbst 0,%0" : : "r"(a) : "memory");
	__asm__ volatile ("sync" : : : "memory");
	for (a = (unsigned long)p & ~127UL; a < (unsigned long)(p + n); a += 128)
		__asm__ volatile ("icbi 0,%0" : : "r"(a) : "memory");
	__asm__ volatile ("sync; isync" : : : "memory");
}

static int call_at(void *code)
{
	static unsigned long desc[3]; unsigned long r2v;
	int (*f)(void);
	__asm__ volatile("mr %0,2" : "=r"(r2v));
	desc[0] = (unsigned long)code; desc[1] = r2v; desc[2] = 0;
	__asm__ volatile("" : : "r"(desc) : "memory");
	f = (int (*)(void))(void *)desc;
	return f();
}

int ps3_jit_stage_test(int stage)
{
	volatile unsigned int *w;
	int r = -1;
	ps3_jit_log("stage %d: begin\n", stage);
	switch (stage) {
	case 0: /* control: call a real function through a hand-made descriptor */
		r = call_at(*(void **)(void *)ctrl_fn);
		break;
	case 1: /* read back after write to cache, no execution */
		w = (volatile unsigned int *)ps3_jit_buffer;
		w[0] = 0x3860002a; w[1] = 0x4e800020;
		sync_range(w, 2);
		ps3_jit_log("stage 1: readback %08x %08x\n", w[0], w[1]);
		r = (w[0] == 0x3860002a) ? 42 : -2;
		break;
	case 2: /* patch an existing function in place and call it */
		w = (volatile unsigned int *)*(void **)(void *)patch_fn;
		ps3_jit_log("stage 2: patching function at %p (was %08x)\n", (void *)w, w[0]);
		w[0] = 0x3860002a; w[1] = 0x4e800020;
		sync_range(w, 2);
		ps3_jit_log("stage 2: patched, calling\n");
		r = call_at((void *)w);
		break;
	case 3: case 4: case 5: case 6: case 7: {
		static const unsigned off[] = { 0, 0x1000, 0x80000, 0x400000, 0x7ff000 };
		w = (volatile unsigned int *)(ps3_jit_buffer + off[stage - 3]);
		w[0] = 0x3860002a; w[1] = 0x4e800020;
		sync_range(w, 2);
		ps3_jit_log("stage %d: calling cache+%x\n", stage, off[stage - 3]);
		r = call_at((void *)w);
		break;
	}
	default: return -99;
	}
	ps3_jit_log("stage %d: returned %d\n", stage, r);
	return r;
}
