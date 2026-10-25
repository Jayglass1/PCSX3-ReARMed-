#!/usr/bin/env bash
# usage: build_ws.sh <pkg name>
set -e
cd /home/claude/w/wsps3
. ./FLAGS.sh
CF="$CF -fno-strict-aliasing -ffunction-sections -fdata-sections -O2"
I=/home/claude/w/RetroArch/libretro-common/include
mkdir -p obj; rm -f obj/*.o libws.a
SRC="$(ls *.c | grep -v '^lightrec.c$') ppc/pR3000A.c ppc/ppc.c ppc/reguse.c gpulib/gpulib.c gpulib/vout_pl.c SoftGPU/gpulib_if.c SoftGPU/gpuCommon.c SoftGPU/prim.c SoftGPU/soft.c dfsound/adsr.c dfsound/dfspu.c dfsound/dma.c dfsound/freeze.c dfsound/out.c dfsound/registers.c dfsound/reverb.c dfsound/xa.c Gamecube/plugins.c Gamecube/Plugin.c deps/libchdr/deps/zlib-1.3.1/adler32.c deps/libchdr/deps/zlib-1.3.1/crc32.c deps/libchdr/deps/zlib-1.3.1/gzclose.c deps/libchdr/deps/zlib-1.3.1/gzlib.c deps/libchdr/deps/zlib-1.3.1/gzread.c deps/libchdr/deps/zlib-1.3.1/gzwrite.c deps/libchdr/deps/zlib-1.3.1/inffast.c deps/libchdr/deps/zlib-1.3.1/inflate.c deps/libchdr/deps/zlib-1.3.1/inftrees.c deps/libchdr/deps/zlib-1.3.1/zutil.c deps/libchdr/deps/zlib-1.3.1/deflate.c deps/libchdr/deps/zlib-1.3.1/trees.c deps/libchdr/deps/zlib-1.3.1/compress.c deps/libchdr/src/libchdr_bitstream.c deps/libchdr/src/libchdr_cdrom.c deps/libchdr/src/libchdr_chd.c deps/libchdr/src/libchdr_flac.c deps/libchdr/src/libchdr_huffman.c deps/libchdr/deps/lzma-19.00/LzmaEnc.c deps/libchdr/deps/lzma-19.00/LzFind.c deps/libchdr/deps/lzma-19.00/Sort.c deps/libchdr/deps/zstd-1.5.6/lib/common/xxhash.c deps/libchdr/deps/lzma-19.00/Alloc.c deps/libchdr/deps/lzma-19.00/LzmaDec.c deps/libchdr/deps/lzma-19.00/Bra86.c deps/libchdr/deps/lzma-19.00/BraIA64.c deps/libchdr/deps/lzma-19.00/Delta.c deps/libchdr/deps/lzma-19.00/CpuArch.c deps/libchdr/deps/zstd-1.5.6/lib/common/entropy_common.c deps/libchdr/deps/zstd-1.5.6/lib/common/error_private.c deps/libchdr/deps/zstd-1.5.6/lib/common/fse_decompress.c deps/libchdr/deps/zstd-1.5.6/lib/common/zstd_common.c deps/libchdr/deps/zstd-1.5.6/lib/decompress/huf_decompress.c deps/libchdr/deps/zstd-1.5.6/lib/decompress/zstd_ddict.c deps/libchdr/deps/zstd-1.5.6/lib/decompress/zstd_decompress.c deps/libchdr/deps/zstd-1.5.6/lib/decompress/zstd_decompress_block.c"
for f in $SRC; do
  o=obj/$(echo $f|tr / _).o; x=""
  [ "$f" = Gamecube/Plugin.c ] && x="-D__GX__"
  case $f in deps/libchdr/*) x="-D_7ZIP_ST -DZSTD_DISABLE_ASM -DZSTD_STATIC_LINKING_ONLY -DHAVE_STDINT_H -Ideps/libchdr/include -Ideps/libchdr/deps/zstd-1.5.6/lib -Ideps/libchdr/deps/zstd-1.5.6/lib/common" ;; esac
  ppu-gcc $CF -DUSE_LIBCHDR -I$I $x -c $f -o $o 2>>/home/claude/w/ws-build.err || { echo FAIL $f; exit 1; }
done
ppu-ar rcs libws.a obj/*.o
cd /home/claude/w/RetroArch
U=pkg/psl1ght/pkg/USRDIR
cp /home/claude/w/wsps3/libws.a libretro_psl1ght.a
rm -f retroarch_psl1ght.elf retroarch_psl1ght*.self retroarch_psl1ght.pkg $U/cores/*.SELF
make -f Makefile.psl1ght -j2 retroarch_psl1ght.elf > /home/claude/w/ra-$1.log 2>&1
python3 /home/claude/w/patch_rwx.py retroarch_psl1ght.elf
make -f Makefile.psl1ght -j2 retroarch_psl1ght.self >> /home/claude/w/ra-$1.log 2>&1
make -f Makefile.psl1ght create-core CORE_PATH=$U/cores/pcsx_rearmed_libretro.SELF >/dev/null 2>&1
make -f Makefile.psl1ght.salamander pkg > /home/claude/w/ra-$1-pkg.log 2>&1
cp retroarch_psl1ght.pkg /tmp/claude-0/-home-claude/b960e41b-a814-593e-b81f-78a1cf827a7a/scratchpad/$1.pkg
ls -la /tmp/claude-0/-home-claude/b960e41b-a814-593e-b81f-78a1cf827a7a/scratchpad/$1.pkg
