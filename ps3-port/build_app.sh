#!/usr/bin/env bash
# usage: build_app.sh <pkgname>
set -e
. /home/claude/w/wsps3/FLAGS.sh
cd /home/claude/w/wsps3
CF2="$CF -fno-strict-aliasing -ffunction-sections -fdata-sections -O2"
ppu-gcc $CF2 -I/home/claude/w/RetroArch/libretro-common/include -c ps3_glue.c -o obj/ps3_glue.c.o
ppu-gcc $CF2 -I/home/claude/w/RetroArch/libretro-common/include -c ps3_jit.c -o obj/ps3_jit.c.o
ppu-ar rcs libws.a obj/*.o 2>/dev/null; cp obj/ps3_glue.c.o obj/ps3_jit.c.o /tmp/ 2>/dev/null
cd /home/claude/w/wsapp; mkdir -p build/pkg/USRDIR
ppu-gcc -O2 -w -mcpu=cell -I$PSL1GHT/ppu/include -I/home/claude/w/RetroArch/libretro-common/include -c main.c -o build/main.o
ppu-gcc -mcpu=cell -Wl,--no-multi-toc -Wl,--gc-sections -L$PSL1GHT/ppu/lib build/main.o ${LIBWS:-/home/claude/w/wsps3/libws.a} -lrsx -lgcm_sys -lio -laudio -lsysutil -lsysmodule -lrt -llv2 -lm -o build/wiistation.elf
ppu-strip build/wiistation.elf -o build/ws_stripped.elf
sprxlinker build/ws_stripped.elf
CID=UP0001-WSTN00001_00-0000000000000001
make_self_npdrm build/ws_stripped.elf build/pkg/USRDIR/EBOOT.BIN $CID
sfo --title "WiiStation PS3" --appid WSTN00001 -f $PS3DEV/bin/sfo.xml build/pkg/PARAM.SFO
cp $PS3DEV/bin/ICON0.PNG build/pkg/ICON0.PNG
pkg --contentid $CID build/pkg/ build/$1.pkg
cp build/$1.pkg ${OUT:-/tmp}/$1.pkg
ls -la build/$1.pkg
