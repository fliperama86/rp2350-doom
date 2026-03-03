# Investigation Progress

## Summary
Two root causes identified and resolved for HDMI sync drops:

1. **Core 1 stack overflow**: `PICO_CORE1_STACK_SIZE=0x4f8` (1272 bytes) in SCRATCH_X was far too small. `pd_core1_loop()`'s deep render call chain + DMA ISR frame overflowed the stack, corrupting Core 1 state and killing HDMI output after 1-2 seconds. **Fix**: moved Core 1 stack to 8KB in main SRAM via `multicore_launch_core1_with_stack()`.

2. **Core 1 bus contention**: Even with sufficient stack, `pd_core1_loop()`'s heavy SRAM access (frame buffers, visplane rendering) caused bus contention with the DMA ISR's timing-critical HSTX FIFO feeds, producing micro-glitches that accumulated into sync drops after ~20 seconds. **Fix**: `pd_core1_loop()` disabled on Core 1 entirely. Core 1 is now dedicated to HDMI output only (DMA ISR + lightweight video handoff).

## Pixel Clock
- System clock = **252 MHz** (set in `i_main.c:83`)
- `MODE_HSTX_CLK_DIV=2` → clk_hstx = 126 MHz → pixel clock = 126/5 = **25.2 MHz** (standard 640x480@60Hz)
- DIV=1 produces 50.4 MHz (no signal) — do NOT use

## Diagnostics Completed
- Stage 1: color bars confirmed TMDS operational
- Stage 2: diagnostic bars confirmed frame pipeline handoff works end-to-end (publish, consume, video type transition all active)
- Stage 2 handoff fix: `pd_core1_loop()` changed to non-blocking, `hdmi_diag_service_video_handoff()` added for deferred VSYNC→frame consumption
- Stage 3 solid color: confirmed scanline callback itself is not the timing bottleneck
- Stack overflow isolation: disabling `pd_core1_loop()` → indefinitely stable HDMI; re-enabling with 8KB stack → ~20s with glitches (bus contention)

## Architecture Decision
Core 1 is exclusively reserved for HDMI output:
- DMA ISR (`dma_irq_handler`) at priority 0
- VSYNC callback (deferred to background task)
- `new_frame_stuff()` / `hdmi_diag_service_video_handoff()` (lightweight frame handoff)
- `video_output_core1_loop()` main loop

Core 0 handles all Doom rendering single-threaded. The game will be slower without Core 1 render assistance, but HDMI remains stable. Future optimization: pre-render scanlines on Core 0 into a ready buffer that Core 1's DMA ISR can memcpy from.

## Key Code Changes
- `i_video.c`: `core1_background_task()` no longer calls `pd_core1_loop()`
- `i_video.c`: Core 1 launched with `multicore_launch_core1_with_stack()` using 8KB main-SRAM buffer
- `i_main.c`: early `I_InitGraphics()` for `PICODOOM_HDMI_DIAG_STAGE >= 2`
- `CMakeLists.txt`: `MODE_HSTX_CLK_DIV=2` (correct for 252 MHz sys_clk)
- `pd_render.cpp`: `USE_CORE1_RENDER=0` disables all Core 1 render synchronization (`core1_wake`/`core1_done`/`core0_done`, `USE_CORE1_FOR_FLATS=0`, `USE_CORE1_FOR_REGULAR=0`)
- `i_video.c`: scanline callback falls back to inline green fill when `display_video_type == VIDEO_TYPE_NONE`

## Current Status (2026-03-03)
**Doom game image appeared on HDMI for ~1 second**, then sync dropped.
- Inline constant fills (green/gray) are indefinitely stable.
- Frame handoff (`new_frame_stuff()` in background task) is stable.
- Actual Doom scanline rendering (palette conversion + 2x expansion from frame buffer) works but drops after ~1s.
- LED keeps blinking after signal loss → Core 1 DMA ISR is still running, not a crash.
- Suspected cause: scanline callback SRAM reads (frame buffer) in ISR context compete with Core 0's simultaneous SRAM writes during rendering.

## Next Steps
- Test if sync drop correlates with Core 0 rendering activity (freeze Core 0 after first frame → does HDMI stay stable?)
- If SRAM read contention is confirmed, try pre-rendering scanlines on Core 0 into a dedicated line buffer that the ISR only needs to memcpy
- Consider 240p mode (`VIDEO_MODE_320x240`) to double the per-scanline time budget
- Re-enable audio (HDMI data islands) once video is stable
