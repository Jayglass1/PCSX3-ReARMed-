# WiiStation PS3 port (PCSX-ReARMed lineage)

Standalone PS3 homebrew build (PSL1GHT, ppu-gcc 7.2.0).

- `wsps3/`  emulator core: PPC dynarec, SoftGPU, CHD (libchdr), SPU-assisted pieces
- `wsapp/`  PS3 front-end (`main.c`: menu, RSX scaler, FXAA pass in `fx/`)
- `build_ws.sh` builds `libws.a`; `build_app.sh <name>` builds the `.pkg`
- `releases/` packages: v98 (FXAA default), v104 (SPU polygon renderer, FXAA 2x-16x, Turbo CD; tested good on hardware)
- `host_test/` x86 test rig; `test_spu.c` checks the SPU polygon renderer against SoftGPU pixel for pixel

The SPU polygon renderer (`wsps3/spu/`) is on by default (Start toggles it); fights run at full speed.

## Lightrec CPU option (source only, not yet tested on hardware)

`CPU: Interpreter / Dynarec / Lightrec` (Triangle in the menu).
- `wsps3/lightrec.c`: glue (scheduler, memory map, code buffer = the `.text` cache `ps3_jit_buffer`)
- `wsps3/deps/lightrec`: Lightrec (WiiStation copy). `OPT_EARLY_UNLOAD` and `OPT_TRANSFORM_OPS` are **off**:
  `host_test/lr_test.c` found wrong results with them on.
- `wsps3/deps/lightning`: GNU Lightning (pcercuei fork, see `REVISION`), built with `HAVE_MMAP=0`;
  `jit_flush` for the PS3 uses dcbst/icbi directly.
- `wsps3/ps3_lr_stubs.c`: weak libc shims (mmap/isatty/sysconf/umask) for the linker.
- A `wiistation_lightrec.mark` file is written while a Lightrec game runs; if it is still there at the next
  start the menu notes that Lightrec did not exit cleanly.
- `host_test/build_lr_test.sh` builds `lr_test` (x86): random MIPS programs, Lightrec JIT vs Lightrec interpreter.
  Run `CLS=15 ./lr_test 4000` (also `LR32=1` to put the code buffer below 4 GB like the PS3).
