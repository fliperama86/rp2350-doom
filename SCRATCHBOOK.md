# Scratchbook

## Context
- Project: `rp2350-doom`
- Board tested: RP2350 (`pico2`)
- Symptom: firmware flashed but no video output, while `pico_hdmi` `bouncing_box` demo works on same wiring.

## Key Findings
- `Debug` firmware for `doom_tiny_usb` was too large and overlapped WHX payload space.
- `doom_tiny_usb` requires WHX at `0x10042000`.
- A `Debug` build we produced ended near `0x1004cb50`, which overlaps `0x10042000` and can corrupt runtime.
- `MinSizeRel` build fixes this size issue; `doom_tiny_usb` ended near `0x1003f9e4` (safe below `0x10042000`).
- `picotool -f/-F` only works when app is running compatible USB stdio; otherwise BOOTSEL mode is required.
- `picotool` in this environment was intermittently unstable; BOOTSEL mount/copy was also flaky during large copies.

## Video Driver Lessons
- Keep heavy per-frame work out of VSYNC/DMA IRQ context.
- Defer frame update work from VSYNC callback to background task path.
- For sync compatibility with picky sinks, DVI mode (no HDMI data islands/audio) may be more reliable.
- `video_output_init()` should match actual output timing mode (640x480 here), not source buffer size.

## Known-Good Build/Flash Targets
- Build type for on-device firmware: `MinSizeRel`.
- Target: `doom_tiny_usb`.
- WHX file: `doom1.whx`.
- WHX load address for `doom_tiny_usb`: `0x10042000`.

## Recommended Flash Sequence (Official picotool flow)
1. Put board in BOOTSEL mode.
2. `picotool load -v build-min/src/doom_tiny_usb.uf2`
3. `picotool load -v -t bin doom1.whx -o 0x10042000`
4. `picotool reboot -a`

## Diagnostic Ladder (Implemented)
- Build knobs in `src/pico/CMakeLists.txt`:
  - `PICODOOM_HDMI_DIAG_STAGE`:
    - `0` normal
    - `1` color bars only (HDMI pipeline check)
    - `2` Doom base frame only, no overlays (scanline budget check)
    - `3` normal with deferred VSYNC work (same rendering path, safer scheduling)
  - `PICODOOM_HDMI_DVI_MODE`: `1` DVI(video-only) or `0` HDMI(data islands/audio)

- Example builds:
  - Stage 1 bars:
    - `cmake -S . -B build-min -DCMAKE_BUILD_TYPE=MinSizeRel -DPICO_BOARD=pico2 -DPICO_SDK_PATH=/Users/dudu/pico-sdk -DPICODOOM_HDMI_DIAG_STAGE=1 -DPICODOOM_HDMI_DVI_MODE=1`
  - Stage 2 Doom base:
    - `cmake -S . -B build-min -DCMAKE_BUILD_TYPE=MinSizeRel -DPICO_BOARD=pico2 -DPICO_SDK_PATH=/Users/dudu/pico-sdk -DPICODOOM_HDMI_DIAG_STAGE=2 -DPICODOOM_HDMI_DVI_MODE=1`
  - Stage 3/normal path:
    - `cmake -S . -B build-min -DCMAKE_BUILD_TYPE=MinSizeRel -DPICO_BOARD=pico2 -DPICO_SDK_PATH=/Users/dudu/pico-sdk -DPICODOOM_HDMI_DIAG_STAGE=3 -DPICODOOM_HDMI_DVI_MODE=1`
  - Build target:
    - `cmake --build build-min -j8 --target doom_tiny_usb`

## Guardrails
- Always check UF2 metadata before flashing:
  - `picotool info -a build-min/src/doom_tiny_usb.uf2`
- Ensure binary end address is below WHX base address.
- Avoid using `Debug` for device firmware unless explicitly validating memory map headroom.

## Latest Session Notes (2026-03-03)
- Stage 1 (`PICODOOM_HDMI_DIAG_STAGE=1`, DVI mode on) produced visible color bars on target display.
- This confirms TMDS/pixel-clock/output timing path can lock on this wiring/display stack.
- Initial Stage 2 (`PICODOOM_HDMI_DIAG_STAGE=2`) attempt showed black/no sync.
- Stage 2 code was then hardened to:
  - initialize graphics earlier in boot for stage >= 2,
  - make `I_InitGraphics()` idempotent,
  - show bars when `VIDEO_TYPE_NONE` even in Stage 2.
- Updated Stage 2 firmware + `doom1.whx` was rebuilt/flashed with `picotool` using MinSizeRel.
- User later reported bars visible in Stage 2 fallback path (`display_video_type == VIDEO_TYPE_NONE`), confirming scanline callback path is alive while Doom frame type has not switched in.
- Added a deeper Stage 2 diagnostic:
  - When `display_video_type == VIDEO_TYPE_NONE`, screen now shows 4 wide state bars (not rainbow bars):
    1. VSYNC callback activity
    2. `pd_end_frame()` activity
    3. frame publish (`sem_release(&render_frame_ready)`)
    4. frame consume (`new_frame_stuff` taking `render_frame_ready`)
  - Bar color meaning: red = counter never seen, bright/dim = counter active and toggling.
- Rebuilt `doom_tiny_usb` successfully (MinSizeRel), UF2 binary end `0x1003f058` (safe below WHX base `0x10042000`).
- Flash retry succeeded end-to-end:
  - `picotool load -v -f build-min/src/doom_tiny_usb.uf2` -> OK (verified)
  - `picotool load -v -t bin doom1.whx -o 0x10042000` -> OK (verified)
  - `picotool reboot -a` -> rebooted to application mode
- Added second-pass Stage 2 counter mapping and reflashed (verified):
  - Bar 1: VSYNC callback activity
  - Bar 2: `D_DoomLoop` heartbeat
  - Bar 3: `pd_begin_frame()` entry
  - Bar 4: `pd_end_frame()` entry
  - Flash sequence repeated successfully (`UF2 + WHX + reboot`).
- Added third-pass Stage 2 boot checkpoint mapping and reflashed (verified):
  - Bar 1: VSYNC callback activity
  - Bar 2: `main()` reached point just before `D_DoomMain()`
  - Bar 3: `D_DoomMain()` entry
  - Bar 4: after first IWAD add (`D_AddFile`/WAD init checkpoint)
  - Flash sequence repeated successfully (`UF2 + WHX + reboot`).
- User observation with third-pass mapping: still only Bar 1 active (Bars 2-4 red).
- Added fourth-pass Stage 2 mapping focused on `main()` checkpoints and reflashed (verified):
  - Bar 1: VSYNC callback activity
  - Bar 2: after early `I_InitGraphics()` in `main()` (diag bring-up path)
  - Bar 3: after `I_Init()` in `main()`
  - Bar 4: immediately before `D_DoomMain()` call
  - Flash sequence repeated successfully (`UF2 + WHX + reboot`).
- Added fifth-pass Stage 2 mapping focused inside `I_InitGraphics()` itself and reflashed (verified):
  - Bar 1: VSYNC callback activity
  - Bar 2: `I_InitGraphics()` reached pre-wait point (`multicore_launch_core1` done)
  - Bar 3: `I_InitGraphics()` passed `sem_acquire_blocking(&core1_launch)`
  - Bar 4: `I_InitGraphics()` completed (`initialized=true`)
  - Flash sequence repeated successfully (`UF2 + WHX + reboot`).
- Added sixth-pass Stage 2 mapping to bracket `multicore_launch_core1()` and reflashed (verified):
  - Bar 1: VSYNC callback activity
  - Bar 2: in `I_InitGraphics()`, immediately before `multicore_launch_core1(core1)`
  - Bar 3: in `I_InitGraphics()`, immediately after `multicore_launch_core1(core1)` returns
  - Bar 4: in `I_InitGraphics()`, after `sem_acquire_blocking(&core1_launch)` returns
  - Flash sequence completed (`UF2 + WHX + reboot`) after user put board in BOOTSEL.
- User challenged diagnostics; sanity-check firmware (bar2 forced from VSYNC source) showed bars 1+2 active, confirming diagnostic display path and reflashing workflow were functioning.
- Upgraded diagnostics to use watchdog scratch register flags (latched, non-RAM) for bars 2-4 to avoid false negatives from RAM corruption/reset:
  - Bar 2: latched before `multicore_launch_core1(core1)`
  - Bar 3: latched after `multicore_launch_core1(core1)` returns
  - Bar 4: latched after `sem_acquire_blocking(&core1_launch)` returns
  - Reflashed (`UF2 + WHX + reboot`) with this robust mapping.
- Observed issue: watchdog scratch-register latches did not reflect expected progression reliably on this target.
- Switched again to scratch-RAM monotonic marker with inverse canary:
  - Marker 1: before `multicore_launch_core1(core1)`
  - Marker 2: after `multicore_launch_core1(core1)` returns
  - Marker 3: after `sem_acquire_blocking(&core1_launch)` returns
- New firmware built successfully (`binary end 0x1003f1b8`), but flashing is currently blocked because device is not visible in BOOTSEL.
- Device reconnected; scratch-RAM marker build was flashed successfully (`UF2 + WHX + reboot`).
- Diagnostics moved forward to pipeline checkpoints:
  - Marker 1: after early `I_InitGraphics()` in `main`
  - Marker 2: after `I_Init()` in `main`
  - Marker 3: immediately before `D_DoomMain()`
  - Marker 4: `D_DoomMain()` entry
  - Marker 5: after initial WAD add (`D_AddFile`)
  - Marker 6: immediately before `D_DoomLoop()`
  - Marker 7: `D_Display()` entry
  - Marker 8: `pd_end_frame()` entry
- Current Stage 2 bar thresholds:
  - Bar 1 (green): VSYNC live
  - Bar 2 (blue): marker >= 2
  - Bar 3 (cyan): marker >= 5
  - Bar 4 (yellow): marker >= 8
- This build flashed successfully (`UF2 + WHX + reboot`), ready for next observation.
- Refined Stage 2 thresholds to isolate where rendering path stops and reflashed (verified):
  - Bar 1 (green): VSYNC live
  - Bar 2 (blue): marker >= 6 (immediately before `D_DoomLoop()`)
  - Bar 3 (cyan): marker >= 7 (`D_Display()` entered)
  - Bar 4 (yellow): marker >= 8 (`pd_end_frame()` entered)
  - Flash sequence succeeded (`UF2 + WHX + reboot`).
- User observation on that `>=6/7/8` build: only Bar 1 flashing green, Bars 2-4 red.
  - Interpretation: execution did not reach marker 6 (`before D_DoomLoop`); current stop is at marker <= 5.
- Remapped Stage 2 thresholds for finer isolation and reflashed (verified):
  - Bar 1 (green): VSYNC live
  - Bar 2 (blue): marker >= 4 (`D_DoomMain()` entry)
  - Bar 3 (cyan): marker >= 5 (after initial `D_AddFile()` checkpoint)
  - Bar 4 (yellow): marker >= 6 (immediately before `D_DoomLoop()`)
  - Flash sequence succeeded (`UF2 + WHX + reboot`).
- User observation on that `>=4/5/6` build:
  - Flashing green, lit blue, lit cyan, lit red.
  - Interpretation: marker reached 5 but not 6; stall is after initial WAD add and before `D_DoomLoop()` marker.
- Added a new "binary-search" Stage 2 checkpoint map inside the marker-5->marker-6 gap and reflashed (verified):
  - Marker 6: after WAD/deh command-line parse section (`W_ParseCommandLine` convergence point)
  - Marker 7: after `I_InitMusic()`
  - Marker 8: after `S_Init()`
  - Bars now map to:
    - Bar 1 (green): VSYNC live
    - Bar 2 (blue): marker >= 6
    - Bar 3 (cyan): marker >= 7
    - Bar 4 (yellow): marker >= 8
  - Flash sequence succeeded (`UF2 + WHX + reboot`).
- User observation on that map:
  - Flashing green, lit blue, lit red, lit red.
  - Interpretation: marker reached 6 but not 7; stall occurs in/around the early machine/audio init block before the post-`I_InitMusic` checkpoint.
- Added focused audio-block diagnostics and reflashed (verified):
  - Marker 7: immediately before `I_InitSound()/I_InitMusic()` block (after timer/joystick init)
  - Marker 8: immediately after `I_InitSound()/I_InitMusic()` block
  - Marker 9: after `S_Init()`
  - Stage 2 diagnostic build now **skips audio init** (`I_InitSound/I_InitMusic`) to test the no-audio path while keeping marker flow:
    - `#if PICODOOM_HDMI_DIAG_STAGE >= 2` skip sound bring-up
  - Bars now map to:
    - Bar 1 (green): VSYNC live
    - Bar 2 (blue): marker >= 7
    - Bar 3 (cyan): marker >= 8
    - Bar 4 (yellow): marker >= 9
  - Flash sequence succeeded (`UF2 + WHX + reboot`).
- User observation on that map:
  - Flashing green, lit blue, lit cyan, lit yellow.
  - Interpretation: startup progressed cleanly through marker 9 with audio skipped; no stall in the previously suspected init block on this no-audio diagnostic path.
- Advanced diagnostics to frame pipeline and reflashed (verified):
  - Marker 10: immediately before `D_DoomLoop()`
  - Marker 11: `D_Display()` entry
  - Marker 12: frame publish point in `pd_end_frame()` (just before `sem_release(&render_frame_ready)`)
  - Bars now map to:
    - Bar 1 (green): VSYNC live
    - Bar 2 (blue): marker >= 10
    - Bar 3 (cyan): marker >= 11
    - Bar 4 (yellow): marker >= 12
  - Flash sequence succeeded (`UF2 + WHX + reboot`).
- User observation on that map:
  - Flashing green, lit blue, lit cyan, lit yellow.
  - Interpretation: `D_DoomLoop`, `D_Display`, and frame publish all execute.
  - Remaining suspect narrowed to frame handoff/consume (`render_frame_ready` -> `new_frame_stuff`) and/or mode switch from `VIDEO_TYPE_NONE`.
- Reworked Stage 2 diagnostics to directly show producer/consumer handoff status and reflashed (verified):
  - Stage 2 now always shows diagnostic bars (does not auto-switch to game content).
  - Bars now map to:
    - Bar 1: VSYNC activity counter
    - Bar 2: frame publish counter (`hdmi_diag_pd_publish_count`)
    - Bar 3: frame consume counter (`hdmi_diag_frameconsume_count`)
    - Bar 4: video type state:
      - red = `display_video_type==NONE && next_video_type==NONE`
      - yellow = `display_video_type==NONE && next_video_type!=NONE`
      - green = `display_video_type!=NONE`
  - Flash sequence succeeded (`UF2 + WHX + reboot`).
- User observation on that handoff map (verbatim): `Flashing green`, `unlit blue`, `re[d]`, `lit yellow`.
  - Working interpretation: publish not clearly visible (possibly very dim), consume red/off, and video-type state yellow (`next_video_type!=NONE` while `display_video_type==NONE`).
  - This points to a handoff stall where rendered frames are queued/typed by producer but not consumed into display state.
- Implemented candidate fix for handoff starvation and reflashed (verified):
  - `pd_core1_loop()` changed from blocking wait on `core1_wake` to non-blocking poll (`sem_acquire_timeout_ms(..., 0)`), returning immediately when no render work is queued.
  - Rationale: `pd_core1_loop()` runs inside HDMI background task; blocking there can starve `new_frame_stuff()` from consuming `render_frame_ready`.
  - Also increased dim colors for publish/consume bars to improve readability:
    - publish dim `0x0010` (was very dark `0x0008`)
    - consume dim `0x03ef` (brighter cyan)
  - Flash sequence succeeded (`UF2 + WHX + reboot`).
- User observation after that fix:
  - Flashing green, blue, lit red, lit yellow.
  - Interpretation: publish appears present but consume still absent; `next_video_type` set while `display_video_type` remains `NONE`.
- Added deeper deadlock-break fix and reflashed (verified):
  - Introduced `hdmi_diag_service_video_handoff()` in HDMI video path to service `new_frame_stuff()` from core1 whenever VSYNC pending.
  - Core1 background task now calls this helper directly.
  - `SafeUpdateSound()` (used inside core1 wait loops) now also calls this helper, so video handoff continues even while waiting on render semaphores (`core0_done` / work semaphores).
  - Rationale: avoid starvation where core1 waits in render sync paths while display handoff semaphores (`display_frame_freed`/`render_frame_ready`) stop progressing.
  - Flash sequence succeeded (`UF2 + WHX + reboot`).
- User observation after deeper fix:
  - `Flashing green`, `Flashing blue`, `Flashing cyan`, `Lit green`.
  - Interpretation: VSYNC, frame publish, and frame consume are all active, and `display_video_type != NONE`; frame pipeline handoff is now functioning end-to-end.
  - Additional user symptom: HDMI sync drops after ~30 seconds with minor glitches (likely timing headroom/critical-path load issue).
- Reduced diagnostic overhead and moved to runtime stability test build:
  - Stage 2 remains dedicated diagnostics-bar mode (`PICODOOM_HDMI_DIAG_STAGE == 2`).
  - Stage 3 no longer forces diagnostics bars; returns to normal scanline rendering path.
  - Early `I_InitGraphics()` bring-up in `main()` now only for Stage 2.
  - Reconfigured and flashed with:
    - `PICODOOM_HDMI_DIAG_STAGE=3`
    - `PICODOOM_HDMI_DVI_MODE=1`
  - Flash sequence succeeded (`UF2 + WHX + reboot`).
- User observation on initial Stage 3 runtime build:
  - Signal appears for ~1 second (possibly a cyan frame), then sync drops.
  - Interpretation: frame handoff is fixed, but full Stage 3 rendering path is still too heavy/unstable on current timing budget.
- Applied Stage 3 stability reductions and reflashed (verified):
  - In `new_frame_stuff()`, Stage 3 now coerces `VIDEO_TYPE_DOUBLE`/`VIDEO_TYPE_WIPE` to `VIDEO_TYPE_SINGLE`.
  - In scanline callback, overlay composition is disabled for Stage 3 (`display_video_type >= FIRST_VIDEO_TYPE_WITH_OVERLAYS` now only when diag stage `< 3`).
  - Goal: minimize critical scanline-path work while preserving basic Doom video output.
  - Flash sequence succeeded (`UF2 + WHX + reboot`).
- Applied additional scanline-path optimization and reflashed (verified):
  - Added per-line-pair cache in scanline callback: odd output lines now reuse the previous even line buffer for vertical 2x scaling.
  - This avoids recomputing palette conversion/scanline composition twice for each Doom source line.
  - Also avoids duplicate per-doom-line overlay/list churn on repeated line indices.
  - Goal: cut active scanline CPU load roughly in half and improve timing margin.
  - Flash sequence succeeded (`UF2 + WHX + reboot`).
- Added frame-rate LED heartbeat:
  - `PICO_DEFAULT_LED_PIN` now initialized with `gpio_init`/`gpio_set_dir` inside `I_InitGraphics()`.
  - `hdmi_vsync_callback()` toggles that pin every VSYNC, so the RGB LED now blinks once per frame, making flicker visible.
  - Flash sequence succeeded (same `UF2 + WHX + reboot`).
- LED now blinks every 30 frames to reduce toggle frequency when the display is stuck:
  - `hdmi_vsync_callback()` increments `hdmi_blink_counter` and toggles the LED only when the counter reaches 30, then resets the counter.
  - This gives a visibly slower heartbeat tied to the video frame rate, which should be easier to watch while the output signal is unstable.
  - Flash sequence succeeded (`UF2 + WHX + reboot`).
- Added optional solid-color mode for stress testing:
  - New `PICODOOM_SOLID_COLOR` CMake knob takes an RGB565 value; compiling with e.g. `-DPICODOOM_SOLID_COLOR=0x07E0` forces `hdmi_scanline_callback` to fill that color each active line so you can validate HDMI timing while Doom logic keeps running.
  - Solid-color mode runs even during Stage 3; leave the knob at `0` to render normal Doom frames.
- Built/flashed the solid-color test (CMake: `PICODOOM_HDMI_DIAG_STAGE=3`, `PICODOOM_HDMI_DVI_MODE=1`, `PICODOOM_SOLID_COLOR=0x8410` for neutral gray) so the HDMI backend is only doing a simple memcpy each scanline; flash sequence succeeded (`UF2 + WHX + reboot`).

## Current Triage Interpretation
- If Stage 2 now shows bars: fault is in Doom frame publication path (buffer/state not becoming visible), not raw HDMI link.
- If Stage 2 is still black/no sync: fault is earlier in Stage 2 bring-up path or mode/config transition before fallback rendering.

## 2026-03-03 Observations
- Cross-checked ~/Projects/neogeo/neopico-hd docs for resource allocation; their Core 0 owns the 6 MHz PIO1 capture pipe with DMA ping-pong buffers, while Core 1 exclusively drives HSTX plus the audio SRC + Data-Island pipeline, and every timing-sensitive scanline action lives in SCRATCH_X.  This confirms the need to keep HDMI-critical work fenced on Core 1 here as well.
- `pico_hdmi` already claims DMA 0/1 for the HSTX FIFO (per `video_output_rt.c`) and runs the high-priority background task there; nothing else in this repo uses those channels, so the stall must be in the Doom <-> scanline handoff rather than a DMA conflict.

## Root Cause Investigation (continued 2026-03-03)

### Pixel Clock Clarification
- System clock is **252 MHz** (not 126 MHz as previously assumed).
- `MODE_HSTX_CLK_DIV=2` → clk_hstx = 126 MHz → pixel clock = 126/5 = **25.2 MHz** → standard 640x480@60Hz. DIV=2 was CORRECT all along.
- Briefly tested `MODE_HSTX_CLK_DIV=1` which produced 50.4 MHz pixel clock (no signal). Reverted.

### Solid-Color Test (DIV=2, Stage 3)
- Solid-color (0x8410 gray) with Stage 3 did NOT produce stable signal.
- Display analyzer showed: PCLK 25.205 MHz (valid), VSync valid, **HSync ERROR**, Total/Active H: 7712/6912 (should be 800/640).
- The 7712-pixel line = DMA pipeline out of sync, repeating stale transfers.

### Core 1 Stack Overflow Discovery
- Stage 2 (diagnostic bars) also dropped sync after 1-2 seconds — not just Stage 3.
- `PICO_CORE1_STACK_SIZE=0x4f8` (1272 bytes) in SCRATCH_X was far too small.
- `pd_core1_loop()` on Core 1 has deep render call chains; when DMA ISR fires mid-render, combined stack overflows.
- **Proof**: disabling `pd_core1_loop()` entirely → HDMI stable indefinitely (but Doom can't render frames).
- Increased stack to 2KB → stable ~20s (better but still drops).
- Moved stack to 8KB in main SRAM via `multicore_launch_core1_with_stack()` → still drops ~20s with visible glitches.

### Bus Contention Discovery
- 8KB stack eliminated the stack overflow, but sync still drops after ~20s with micro-glitches.
- The glitches indicate **bus contention**: `pd_core1_loop()`'s heavy SRAM access (frame buffers, visplane data) competes with the DMA ISR's timing-critical HSTX FIFO feeds.
- This matches NeoPico-HD's architecture docs: Core 1 must be dedicated to HDMI output only, no heavy computation alongside it.

### Fix Applied
- `pd_core1_loop()` permanently disabled on Core 1.
- Core 1 background task now only runs `hdmi_diag_service_video_handoff()` (lightweight frame handoff).
- Core 1 stack moved to 8KB in main SRAM (prevents stack overflow from `new_frame_stuff()` + DMA ISR).
- Early `I_InitGraphics()` for Stage >= 2 (same as Stage 2 proven path).
- Doom rendering now single-threaded on Core 0 (slower framerate but stable HDMI).

### Incremental ISR Isolation Tests (continued)

**Test A: Inline green fill + empty background task** → STABLE solid green indefinitely.
- Proves DMA ISR + scanline callback with register-only writes is rock solid.

**Test B: Inline green fill + `hdmi_diag_service_video_handoff()` in background** → STABLE.
- Proves background task frame handoff (incl. `new_frame_init_overlays_palette_and_wipe()` in flash) does not disturb timing.

**Test C: `solid_line` memcpy fill + handoff** → BLACK + periodic drops.
- `solid_line` buffer was all zeros (init may have been overwritten or section placement issue).
- Also had drops — but since content was black, unclear if drops were from the memcpy itself.

**Test D: Inline gray (0x8410) fill + handoff** → STABLE solid gray.
- Same as Test B but with different constant. Confirms inline fill from any constant works.

**Test E: `USE_CORE1_RENDER=0` (Core 0 single-threaded) + no solid color** → NO SIGNAL (LED still blinks).
- Core 1 alive but display can't lock. Likely scanline callback overrunning budget or crash in rendering path.

**Test F: Green fallback (VIDEO_TYPE_NONE) → transition to Doom rendering** → SAW DOOM IMAGE FOR ~1 SECOND then signal dropped.
- Green showed first, then actual game appeared briefly.
- Proves: palette conversion + 2x expansion works, frame buffer reads work, but sustained rendering drops sync.
- Cause: likely scanline callback reading from SRAM frame buffers during ISR competes with Core 0 writes.

### Code Changes Made
- `pd_render.cpp`: Added `USE_CORE1_RENDER=0` guard. When 0: `USE_CORE1_FOR_FLATS=0`, `USE_CORE1_FOR_REGULAR=0`, and all `core1_wake`/`core0_done`/`core1_done` sync points are skipped. Core 0 does all rendering.
- `i_video.c`: Scanline callback falls back to inline green when `display_video_type == VIDEO_TYPE_NONE`, otherwise runs full Doom rendering path.
- `i_video.c`: Background task calls `hdmi_diag_service_video_handoff()` only.

### Current Build
- `PICODOOM_HDMI_DIAG_STAGE=3`, `PICODOOM_HDMI_DVI_MODE=1`, `PICODOOM_SOLID_COLOR=0x07e0`
- Green fallback + Doom rendering transition
- Investigating why sustained Doom frame rendering drops sync after ~1s.
