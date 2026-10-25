# WiiStation PS3 port (PCSX-ReARMed lineage)

Standalone PS3 homebrew build (PSL1GHT, ppu-gcc 7.2.0).

- `wsps3/`  emulator core: PPC dynarec, SoftGPU, CHD (libchdr), SPU-assisted pieces
- `wsapp/`  PS3 front-end (`main.c`: menu, RSX scaler, FXAA pass in `fx/`)
- `build_ws.sh` builds `libws.a`; `build_app.sh <name>` builds the `.pkg`
- `releases/v98_fxaa_default.pkg` last tested build (FXAA on by default)
- `host_test/` x86 test rig; `test_spu.c` checks the SPU polygon renderer against SoftGPU pixel for pixel

Work in progress: the SPU polygon renderer (`wsps3/spu/`) is verified on the host but not yet enabled in the PS3 build.
