#!/usr/bin/env bash
# Builds the Lightrec PowerPC64 (PS3, ELFv1 big-endian) tests with ppu-gcc and runs them under qemu-ppc64 (user mode).
#   smoke:  BIOS-like program run in block-step mode (the way the PS3 glue boots), checks C-call wrapper + loops
#   random: lr_test.c port, JIT vs interpreter on random MIPS programs
# Needs PS3DEV env (ppu-gcc, ppu-ld) and a qemu-ppc64 binary (QEMU=path).
set -e
H=$(cd "$(dirname "$0")" && pwd); D=$H/../../wsps3/deps; O=${1:-/tmp/lr_ppc_build}; mkdir -p $O
Q=${QEMU:-qemu-ppc64}
CF="-O2 -g -w -fno-builtin -fno-tree-loop-distribute-patterns -DNDEBUG -DWS_PS3 -DHAVE_MMAP=0 -D__BIG_ENDIAN__ -D_GNU_SOURCE -I$D/lightning/include -I$D/lightning/lib -I$D/lightrec -I$D/lightrec/tlsf"
for f in blockcache constprop disassembler emitter interpreter lightrec memmanager optimizer regcache tlsf/tlsf; do ppu-gcc $CF -c $D/lightrec/$f.c -o $O/lr_$(basename $f).o; done
for f in jit_disasm jit_memory jit_note jit_print jit_size lightning; do ppu-gcc $CF -c $D/lightning/lib/$f.c -o $O/lg_$f.o; done
ppu-gcc $CF -c $H/rt.c -o $O/rt.o
LG=$(ppu-gcc -print-libgcc-file-name)
ppu-gcc $CF -c $H/t.c -o $O/t_smoke.o; ppu-gcc $CF -c $H/lr_ppc.c -o $O/t_rand.o
LIBS=$(ls $O/lr_*.o $O/lg_*.o $O/rt.o)
ppu-ld -N -T $H/ls.ld -o $O/smoke.elf $O/t_smoke.o $LIBS $LG
ppu-ld -N -T $H/ls.ld -o $O/random.elf $O/t_rand.o $LIBS $LG
$Q $O/smoke.elf | grep -v '^\[lr\]' | tail -8
$Q $O/random.elf | grep -v '^\[lr\]' | grep -v 'RAM usage\|sub-par\|32-bit LUT' | tail -3
