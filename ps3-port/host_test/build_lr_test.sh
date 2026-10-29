#!/usr/bin/env bash
# Builds the Lightrec-vs-interpreter host test (x86_64) against the vendored Lightning.
set -e
R=$(cd "$(dirname "$0")/.." && pwd)/wsps3/deps
O=${1:-/tmp/lr_test_build}; mkdir -p $O
CF="-O1 -g -w -DHAVE_MMAP=0 -D_GNU_SOURCE -I$R/lightning/include -I$R/lightrec"
for f in blockcache constprop disassembler emitter interpreter lightrec memmanager optimizer reaper recompiler regcache tlsf/tlsf; do
  gcc $CF -c $R/lightrec/$f.c -o $O/lr_$(basename $f).o; done
for f in jit_disasm jit_memory jit_note jit_print jit_size lightning; do
  gcc $CF -I$R/lightning/lib -c $R/lightning/lib/$f.c -o $O/lg_$f.o; done
gcc $CF -I$R/lightrec -c $(dirname "$0")/lr_test.c -o $O/lr_test.o
gcc -o $O/lr_test $O/*.o -lpthread
echo built $O/lr_test
