/* host build of the SPU band renderer (x86) */
#include <stdlib.h>
#include <string.h>
#include "spu/spu_render.h"
void spu_dma_get(void *ls, u64w ea, unsigned size) { memcpy(ls, (void *)(uintptr_t)ea, size); }
void spu_dma_put(void *ls, u64w ea, unsigned size) { memcpy((void *)(uintptr_t)ea, ls, size); }
void host_render(const spu_prim_t *list, unsigned n, void *vram, void *dest, unsigned hi, unsigned nw)
{
    unsigned w;
    for (w = 0; w < nw; w++) sr_render((u64w)(uintptr_t)list, n, (u64w)(uintptr_t)vram, (u64w)(uintptr_t)dest, hi, w, nw);
}
