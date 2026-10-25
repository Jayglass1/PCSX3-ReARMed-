/* PS3 shim for libogc's gccore.h: basic types and cache helpers only */
#ifndef PS3_GCCORE_SHIM_H
#define PS3_GCCORE_SHIM_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef volatile u8 vu8; typedef volatile u16 vu16; typedef volatile u32 vu32;
typedef float f32; typedef double f64;
typedef u32 BOOL;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#ifndef MAXPATHLEN
#define MAXPATHLEN 1024
#endif
#ifdef __cplusplus
extern "C" {
#endif
void DCFlushRange(void *ptr, u32 len);
void DCStoreRange(void *ptr, u32 len);
void DCInvalidateRange(void *ptr, u32 len);
void ICInvalidateRange(void *ptr, u32 len);
#ifdef __cplusplus
}
#endif
#endif
