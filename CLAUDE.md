# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

A fork of [rp2040-doom](https://github.com/kilograham/rp2040-doom) (itself derived from Chocolate Doom) being **ported to the RP2350 (Raspberry Pi Pico 2) with HDMI output**. The upstream project targets RP2040 with VGA (`pico_scanvideo_dpi`) + I2S audio. This fork replaces that with HDMI via the RP2350 **HSTX** peripheral using the `pico_hdmi` library, plus embedded HDMI audio.

`README.md` is the **original rp2040-doom README** and describes the VGA/I2S build — its pinout and `vgaboard` instructions do **not** apply to this fork's HDMI work. For the actual state of this port, read these instead (they are kept current and are the source of truth):
- `INVESTIGATION_PROGRESS.md` — current debugging status, root causes found, next experiments, known-good builds.
- `SCRATCHBOOK.md` — build/flash procedure, diagnostic ladder, session notes.

The port is a **work in progress**: video is mostly working but HDMI sync can drop under live rendering. Much of the recent work is diagnostic, gated behind `PICODOOM_*` CMake options (see below).

## Building (RP2350 / device)

Builds **must** use `MinSizeRel` (`Debug`/`Release` are too large). The canonical device build lives in **`build/`** — `./flash.sh` (no args) configures it with the release flags and builds it. Do NOT create `build-xyz` variant directories; for diagnostic experiments, reconfigure `build/` with different `PICODOOM_*` flags (all configs are documented in `INVESTIGATION_PROGRESS.md`) or use a temp dir and delete it after.

Release configuration (v0.1.2 lineage — see `flash.sh` for the live copy):

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DPICO_SDK_PATH=/Users/dudu/pico-sdk \
  -DPICO_EXTRAS_PATH=/Users/dudu/pico-extras \
  -DPICO_BOARD=pico2 \
  -DPICO_PLATFORM=rp2350-arm-s \
  -DPICO_STDIO_USB=OFF -DPICO_STDIO_UART=ON \
  -DPICODOOM_HDMI_DIAG_STAGE=3 -DPICODOOM_HDMI_DVI_MODE=0 \
  -DPICODOOM_HDMI_LITE=1 -DPICODOOM_EMU8950_ASM=0 -DPICODOOM_DIAG_OVERLAY=0 \
  -DPICODOOM_DOOM_TINY_USB_WAD_ADDR=0x10080000 \
  -DPICODOOM_SKIP_WIPES=1 \
  -DPICODOOM_SYS_CLOCK_KHZ=252000 -DPICODOOM_HDMI_HSTX_CLK_DIV=2 \
  -DPICODOOM_RENDER_THROTTLE_US=0
cmake --build build --target doom_tiny_usb -j
```

- `PICODOOM_EMU8950_ASM=0` is REQUIRED for correct music (the RP2040-era asm corrupts OPL state on RP2350 — see `INVESTIGATION_PROGRESS.md`). `PICODOOM_DIAG_OVERLAY=1` re-enables the on-screen counter overlay for debugging.
- Four device targets exist (`add_doom_tiny` in `src/CMakeLists.txt`): `doom_tiny`, `doom_tiny_usb`, `doom_tiny_nost`, `doom_tiny_nost_usb`. `*_usb` adds USB-keyboard (TinyUSB host); `*_nost` ("non super tiny") uses the larger WHD format for big WADs. `doom_tiny_usb` is the one normally built/flashed here.
- WHX/WHD load addresses (`TINY_WAD_ADDR`): `doom_tiny_usb` uses `PICODOOM_DOOM_TINY_USB_WAD_ADDR` (release: `0x10080000`; the board has 16 MB flash); `doom_tiny`=`PICODOOM_DOOM_TINY_WAD_ADDR` (default `0x10040000`), `*_nost*`=`0x10048000`.
- `pico-extras` is no longer a real dependency of the HDMI path, but `pico_extras_import.cmake` is still included; passing `-DPICO_EXTRAS_PATH` avoids configure noise.

## Flashing

The UF2 contains code only — the game data (`doom1.whx`) must be loaded separately at the WAD address. Use `./flash.sh`:

```sh
./flash.sh build/src/doom_tiny_usb.uf2   # loads UF2 + doom1.whx (address auto-detected from the UF2), then reboots
./flash.sh                               # no arg: rebuilds build/ with the release config, then flashes
```

`flash.sh` reads the WHX address from the UF2's binary_info, so relocated builds flash correctly. The user's `pi flash <file.uf2>` tool can flash the UF2 alone (with retry/reboot) when the WHX is already present at the address the UF2 expects.

**Always verify the UF2 fits before flashing** — confirm the binary end address is below the WHX base:
```sh
picotool info -a build/src/doom_tiny_usb.uf2 | head -55
```

## Native / chocolate-doom build (verification)

Building without `PICO_SDK_PATH` produces a host SDL2 `chocolate-doom` (and the `whd_gen`/`setup` tools) — useful to verify game logic still works:
```sh
mkdir build && cd build && cmake .. && make -j chocolate-doom
```
`whd_gen <wad> <out.whx>` converts a WAD to the compressed WHX format (`-no-super-tiny` for the larger WHD format). Use a release build of `whd_gen` for full sound fidelity.

## Architecture

**CMake layering**: top `CMakeLists.txt` switches on `PICO_SDK_PATH` (Pico build) vs SDL build. `pico_sdk_init()` must run **before** `add_subdirectory(pico_hdmi)`. Per-game sources are `INTERFACE` libraries (`common`, `game`, `doom`, `common_pico`, `render_newhope`, `small_doom_common`); device executables are assembled by `add_doom_tiny()` in `src/CMakeLists.txt`. The renderer is `render_newhope` (`src/pd_render.cpp`).

**Two-core split** (current decision, see `INVESTIGATION_PROGRESS.md`):
- **Core 1** is dedicated to HDMI output only: the `pico_hdmi` DMA ISR feeds the HSTX FIFO, plus lightweight per-frame video handoff. Core 1's stack lives in 8KB of main SRAM (`multicore_launch_core1_with_stack`), *not* SCRATCH_X.
- **Core 0** runs all of Doom single-threaded. Core-1 render assist (`pd_core1_loop`, the `USE_CORE1_*` paths in `pd_render.cpp`) is currently **disabled** — it caused SRAM bus contention with the timing-critical DMA ISR and dropped HDMI sync.

**Video pipeline**: Doom renders a 320x200 8-bit indexed framebuffer. The HDMI scanline path converts palette→RGB565 (`blit.S`'s `palette8to16`) and scales/letterboxes into 640x480@60Hz. The 320→640 horizontal expansion done per-line in software while Core 0 renders is the current sync-stability bottleneck.

**Clocking**: system clock **252 MHz** (`PICODOOM_SYS_CLOCK_KHZ`), `MODE_HSTX_CLK_DIV=2` → clk_hstx 126 MHz → 25.2 MHz pixel clock (standard 640x480@60Hz). Do not use DIV=1 (50.4 MHz, no signal).

**Audio**: HDMI data-island packets (48kHz, 4-sample batches) when `PICODOOM_HDMI_DVI_MODE=0`. DVI mode (`=1`, video-only) is more reliable on picky sinks and is the current default during video debugging.

**Memory**: malloc/calloc/free are wrapped to use the Doom zone allocator (`USE_ZONE_FOR_MALLOC=1`, `PICO_HEAP_SIZE=0`). SCRATCH_X (4KB) and SCRATCH_Y are nearly full from the `pico_hdmi` ISR — keep Doom code/data out of them; use `__not_in_flash_func` for ISR-context code instead.

### Key files (this fork's hot spots)
- `src/pico/i_video.c` (~1500 lines) — the HDMI video driver, Core 1 launch, scanline callback, frame handoff. Most diagnostic logic lives here.
- `src/pico/i_picosound.c` — SFX + HDMI audio output. `pico_hdmi_set_audio_sample_rate()` is declared in `pico_hdmi/include/pico_hdmi/video_output.h`.
- `src/pd_render.cpp` — the renderer (`render_newhope`); holds the `USE_CORE1_*` toggles.
- `src/pico/blit.S` — ARM assembly palette→RGB565 conversion.
- `opl/opl_pico.c` — OPL2 music synthesis (49716Hz). Check `opl/CMakeLists.txt` for stray audio-lib deps.
- `src/i_main.c` — sets the system clock; holds `binary_info` pin declarations (keep in sync with hardware).
- `src/picodoom.h` — shared pico-port declarations.

### `pico_hdmi`
The HDMI library is a **nested checkout at `pico_hdmi/`** (mirror of `/Users/dudu/Projects/pico_hdmi`). It is **intentionally untracked** by the parent repo and carries its own uncommitted diagnostics — **do not delete, overwrite, or commit it**. It is built as an **INTERFACE library** (its `CMakeLists.txt` was overridden); building it static double-compiles SDK INTERFACE sources and causes duplicate-symbol link errors. (Note: `3rdparty/tinyusb` is the only real git submodule; init with `git submodule update --init`, and local-path submodule adds need `-c protocol.file.allow=always`.)

## Diagnostic build options

This fork adds a large set of `PICODOOM_*` CMake cache options (defined and validated in `src/pico/CMakeLists.txt`, propagated as compile defs) to bisect the HDMI sync problem without code edits. The important ones:

- `PICODOOM_HDMI_DIAG_STAGE` (0–3): 0 normal · 1 color bars (TMDS check) · 2 Doom base frame, no overlays · 3 normal with deferred VSYNC work.
- `PICODOOM_HDMI_DVI_MODE` (0/1): 1 = DVI video-only, 0 = HDMI with audio data islands.
- `PICODOOM_HDMI_240P`, `PICODOOM_HDMI_PREPARED_SCANLINES`, `PICODOOM_HDMI_LINE_RING[_DIRECT|_SIZE|_PREPARE|_VBLANK_ONLY]` — scanout strategy experiments (full RGB565 frame vs. pre-expanded line ring vs. direct-from-indexed).
- `PICODOOM_FORCE_SOLID_SCANOUT` / `PICODOOM_HDMI_SOLID_POINTER_TEST` / `PICODOOM_HDMI_NATIVE_POINTER_TEST` — solid/raw scanout controls for isolating contention.
- `PICODOOM_BOOT_TO_E1M1`, `PICODOOM_SKIP_WIPES` — boot straight into gameplay, skip melt wipes.
- `PICODOOM_IDLE_AFTER_FIRST_DISPLAY[_LOAD]`, `PICODOOM_FREEZE_POINT`, `PICODOOM_PUBLISH_FIRST_FRAME_ONLY`, `PICODOOM_WAIT_AFTER_FRAME_PUBLISH`, `PICODOOM_RENDER_THROTTLE_US`, `PICODOOM_FRAME_CAP_FPS` — Core 0 activity controls to test whether sync drops correlate with render load.
- `PICODOOM_SYS_CLOCK_KHZ`, `PICODOOM_HDMI_HSTX_CLK_DIV` — clock tuning.

When adding a new diagnostic, follow the existing pattern: declare a validated `CACHE STRING`, then add it to the `target_compile_definitions(common_pico ...)` block.

## Gotchas

- `arm-none-eabi-gcc` version affects binary size; the build is size-tight. Wrong size → link failure or stack-overflow-induced palette corruption. Upstream pins 13.2.rel1.
- `PICO_USE_STACK_GUARDS=0` is set because the port currently overflows stacks — be wary when changing stack-heavy code paths.
