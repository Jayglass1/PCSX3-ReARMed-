/* Weak libc shims for GNU Lightning / Lightrec on the PS3 (newlib + PSL1GHT lacks
 * some POSIX calls).  Lightning is built with HAVE_MMAP=0 (code buffer is supplied
 * by Lightrec), so mmap & co. are never reached; they exist only so the linker is
 * satisfied.  Weak: a real libc definition wins. */
#include <stddef.h>
#include <errno.h>

#define WEAK __attribute__((weak))

WEAK void *mmap(void *a, size_t l, int p, int f, int fd, long o) { errno = ENOMEM; return (void *)-1; }
WEAK int munmap(void *a, size_t l) { return 0; }
WEAK int mprotect(void *a, size_t l, int p) { return 0; }
WEAK int isatty(int fd) { return 0; }
WEAK long sysconf(int n) { return n == 30 /* _SC_PAGESIZE */ ? 4096 : -1; }
WEAK unsigned umask(unsigned m) { return m; }

/* ---- diagnostics for the Lightrec bring-up (log: /dev_hdd0/tmp/wiistation_app.log) ---- */
#include <stdarg.h>
#include <stdio.h>
extern void ws_log(const char *fmt, ...);
void ws_lr_log(const char *fmt, ...)
{
	char b[300]; va_list a;
	va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
	ws_log("%s", b);
}
void ws_lr_dump(const char *tag, const void *p, unsigned n)
{
	static int emitted;
	const unsigned char *c = p; unsigned i;
	if (emitted++ >= 10) return;          /* dispatcher, wrapper and the first few blocks */
	if (n > 640) n = 640;
	ws_log("LRDUMP %s @%p size=%u\n", tag, p, n);
	for (i = 0; i < n; i += 16) {
		char b[80]; unsigned j, o = 0;
		for (j = 0; j < 16 && i + j < n; j++) o += sprintf(b + o, "%02x", c[i + j]);
		ws_log("%04x: %s\n", i, b);
	}
}
