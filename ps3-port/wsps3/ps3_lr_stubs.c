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
