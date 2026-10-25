#include <gccore.h>
/* PS3 timebase ~79.8 MHz */
static inline u64 ps3_gettb(void){ u64 t; __asm__ volatile("mftb %0":"=r"(t)); return t; }
#define gettime() ps3_gettb()
#define ticks_to_millisecs(t) ((u32)((t)/79800ULL))
#define ticks_to_microsecs(t) ((u32)((t)/80ULL))
#define diff_usec(a,b) ((u32)(((b)-(a))/80ULL))
