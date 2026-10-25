/*
 * ppc definitions v0.5.1
 *  Authors: linuzappz <linuzappz@pcsx.net>
 *           alexey silinov
 */

#ifndef __PPC_H__
#define __PPC_H__

// include basic types
#include "../psxcommon.h"
#include "ppc_mnemonics.h"

#define NUM_HW_REGISTERS 28

/* general defines */
//#define write8(val)  *(u8 *)ppcPtr = val; ppcPtr++;
//#define write16(val) *(u16*)ppcPtr = val; ppcPtr+=2;
//#define write32(val) *(u32*)ppcPtr = val; ppcPtr+=4;
//#define write64(val) *(u64*)ppcPtr = val; ppcPtr+=8;

/* PS3 (ELFv1): a C function pointer is the address of a 3-word descriptor
 * {entry, toc, env}. Load entry and toc from it, call, restore our own r2
 * from the frame slot the entry stub filled (40(r1)). */
#define CALLFunc(FUNC) \
{ \
    u32 _func = (u32)(uintptr_t)(FUNC); \
    ReleaseArgs(); \
    LIW(12, _func); \
    LD(0, 0, 12); \
    LD(2, 8, 12); \
    MTCTR(0); \
    BCTRL(); \
    LD(2, 40, 1); \
}

extern int cpuHWRegisters[NUM_HW_REGISTERS];

extern u32 *ppcPtr;
extern u8  *j8Ptr[32];
extern u32 *j32Ptr[32];

void ppcInit();
void ppcSetPtr(u32 *ptr);
void ppcShutdown();

extern inline void ppcAlign();
u8 dynMemRead8(u32 mem);
u16 dynMemRead16(u32 mem);
u32 dynMemRead32(u32 mem);
void dynMemWrite32(u32 mem, u32 val);

#endif /* __PPC_H__ */





















