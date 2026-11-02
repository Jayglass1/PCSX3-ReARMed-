# PCSX3-ReARMed PS3 port

Standalone PlayStation 3 homebrew build (PSL1GHT, ppu-gcc 7.2.0), based on the PCSX-ReARMed lineage.

- XMB title: `PCSX3-ReARMed`
- PS3 TITLE_ID / PARAM.SFO app ID: `PCSX3REA1`
- Package content ID: `UP0001-PCSX3REA1_00-0000000000000001`.

- `wsps3/`  emulator core: PPC dynarec, SoftGPU, CHD (libchdr), SPU-assisted pieces
- `wsapp/`  PS3 front-end (`main.c`: menu, RSX scaler, FXAA pass in `fx/`)
- `build_ws.sh` builds `libws.a`; `build_app.sh <name>` builds the `.pkg`
- `releases/` packages: v98 (FXAA default), v104 (SPU polygon renderer, FXAA 2x-16x, Turbo CD; tested good on hardware)
- `host_test/` x86 test rig; `test_spu.c` checks the SPU polygon renderer against SoftGPU pixel for pixel

## Automated NPDRM packages

GitHub Actions builds the NPDRM PKG on pushes to `ps3-port`, pull requests targeting
that branch, and manual dispatch. The Actions artifact named
`pcsx3rearmed-npdrm` contains only the generated `.pkg` file; compiler outputs,
logs, EBOOT.BIN, PARAM.SFO, and the package staging directory are never uploaded.

Pushing a version tag matching `v*` (for example, `v1.0.0`) runs the same builder
and creates a GitHub Release containing only the generated `.pkg` asset. If that
tag's release already exists, the workflow replaces the package and removes any
non-PKG assets.

The CI build uses the PS3DEV `nightly-2026-10-12` Linux X64 toolchain bundle on
Ubuntu 24.04 (the bundle requires glibc 2.38) and a pinned RetroArch checkout for
the `libretro-common` headers. The build scripts
retain their existing local defaults, while allowing CI to override `PS3DEV`,
`PSL1GHT`, and `PORTLIBS`.

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

## RSX hardware renderer (experimental)
Menu "GPU" (R3) cycles Software / RSX 1x / 2x / 3x / 4x. The SoftGPU capture stream is replayed as triangles on the RSX
(`wsapp/rsxgpu.c`, shaders in `wsapp/gp/`) into an N-times larger render target; the picture is scaled to the screen
through the FXAA pass. The CPU keeps a 1x VRAM that is refreshed by readback whenever it must read VRAM.
Limitations: no mask-bit test, no dither, no interlace line skipping; texture-window primitives and lines are drawn
on the CPU at 1x (visible as low-res elements at 2x-4x). Disables the SPU renderer and the old 2x option while active.
Log lines starting with `rsx:` go to /dev_hdd0/tmp/wiistation_app.log.
