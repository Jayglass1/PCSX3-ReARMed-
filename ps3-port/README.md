# WiiStation PS3 port (PCSX-ReARMed lineage)

Standalone PS3 homebrew build (PSL1GHT, ppu-gcc 7.2.0).

- `wsps3/`  emulator core: PPC dynarec, SoftGPU, CHD (libchdr), SPU-assisted pieces
- `wsapp/`  PS3 front-end (`main.c`: menu, RSX scaler, FXAA pass in `fx/`)
- `build_ws.sh` builds `libws.a`; `build_app.sh <name>` builds the `.pkg`
- `releases/` packages: v98 (FXAA default), v104 (SPU polygon renderer, FXAA 2x-16x, Turbo CD; tested good on hardware)
- `host_test/` x86 test rig; `test_spu.c` checks the SPU polygon renderer against SoftGPU pixel for pixel

The SPU polygon renderer (`wsps3/spu/`) is on by default (Start toggles it); fights run at full speed.
