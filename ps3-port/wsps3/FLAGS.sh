export PS3DEV=/home/claude/w/ps3dev-install/ps3dev PSL1GHT=/home/claude/w/ps3dev-install/ps3dev
export PORTLIBS=$PS3DEV/portlibs/ppu
export PATH=/home/claude/w/wrap:$PATH:$PS3DEV/bin:$PS3DEV/ppu/bin:$PS3DEV/spu/bin
export LD_LIBRARY_PATH=/home/claude/w/cg/usr/lib64
W=/home/claude/w/wsps3
CF="-O2 -w -fpermissive -I$W/shim -I$W -I$W/deps/lightrec -I$W/deps/libchdr/deps/zlib-1.3.1 -I$PSL1GHT/ppu/include -I$PORTLIBS/include -D__BIG_ENDIAN__ -DCPU_IS_BIG_ENDIAN=1 -DWORDS_BIGENDIAN=1 -DHAVE_LROUND -D_GNU_SOURCE -DNDEBUG -DPS3 -D__GAMECUBE__ -DHW_RVL -D__PSL1GHT__"
