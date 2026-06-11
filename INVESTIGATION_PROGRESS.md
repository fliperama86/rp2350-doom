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

## Resume Notes (2026-05-25)

### Current Git State
- Parent repo last committed baseline: `48f086c0 Stabilize HDMI scanout with RGB565 frame handoff`.
- Nested `pico_hdmi` last committed baseline: `ed25bf8 Add prepared scanline callback path`.
- There are intentional uncommitted diagnostics in both repos.
- Parent repo currently has modified: `flash.sh`, `src/doom/d_main.c`, `src/i_main.c`, `src/pd_render.cpp`, `src/pico/CMakeLists.txt`, `src/pico/i_video.c`, `src/picodoom.h`.
- Nested `pico_hdmi` currently has modified: `CMakeLists.txt`, `include/pico_hdmi/video_output.h`, `src/video_output.c`, `src/video_output_rt.c`.
- Do not delete or overwrite `pico_hdmi/`; it is a nested checkout and is intentionally untracked from the parent repo.

### Flashing
- Doom UF2s advertise `WHX at 0x10042000` but do not contain `doom1.whx`.
- If the WHX is already present and the UF2 binary end is below `0x10042000`, this is enough:
  ```sh
  pi flash build-name/src/doom_tiny_usb.uf2
  ```
- If in doubt, or after flash erase/recovery, use the patched script:
  ```sh
  ./flash.sh build-name/src/doom_tiny_usb.uf2
  ```
  It loads both the UF2 and `doom1.whx` at `0x10042000`.
- Always verify the UF2 before using `pi flash`:
  ```sh
  picotool info -a build-name/src/doom_tiny_usb.uf2 | head -55
  ```

### Important Build Options Added
- `PICODOOM_HDMI_LINE_RING`
- `PICODOOM_HDMI_LINE_RING_DIRECT`
- `PICODOOM_HDMI_LINE_RING_SIZE`
- `PICODOOM_HDMI_LINE_RING_PREPARE`
- `PICODOOM_HDMI_LINE_RING_VBLANK_ONLY`
- `PICODOOM_HDMI_SOLID_POINTER_TEST`
- `PICODOOM_WAIT_AFTER_FRAME_PUBLISH`
- `PICODOOM_IDLE_AFTER_FIRST_DISPLAY`
- `PICODOOM_IDLE_AFTER_FIRST_DISPLAY_LOAD`
- `PICODOOM_SYS_CLOCK_KHZ`
- `PICODOOM_HDMI_HSTX_CLK_DIV`

### Latest Confirmed Observations
- 480p output is 640x480 timing with Doom's 320x200 content scaled in software.
- Current pixel clock setup remains:
  - `PICODOOM_SYS_CLOCK_KHZ=252000`
  - `PICODOOM_HDMI_HSTX_CLK_DIV=2`
  - 25.2 MHz effective pixel clock
- Full UF2 + WHX flash of `build-min/src/doom_tiny_usb.uf2` gives the actual game image, HUD placed correctly, but horizontal line artifacts remain and sync can drop after minutes.
- Static first-frame tests with Core 0 idled are clean and stable. This confirms the game frame data and basic scanout can be correct when Core 0 is quiet.
- Solid scanout while Doom runs live is stable:
  - `build-solid-live-current`: solid blue, sync holds.
  - `build-solid-pointer-live`: solid blue through the pico_hdmi pointer callback, sync holds.
- Full RGB565 prepared-frame memory is suspicious:
  - `build-prepared-solid-pointer-wait`: solid blue through pointer path, with RGB565 frame allocated/built once. User saw solid blue but "glitches quite a bit".
  - BSS comparison:
    - `build-solid-pointer-live`: `bss=293424`
    - `build-prepared-solid-pointer-wait`: `bss=421484`
    - `build-min`: `bss=421484`
  - The 128 KB `hdmi_rgb565_frame` pushes the zone start much higher and appears to make the live renderer less stable even when scanout reads only a solid line.
- Line-ring behavior:
  - `build-line-ring8-idlefirst-activeprep`: Core 0 idles after first frame, continuous 8-line ring prep. Observed `blue -> game frame -> holds sync`.
  - `build-line-ring8-wait-publish-first`: Core 0 waits for first RGB565 build, then resumes Doom while display remains first published frame. Observed `blue -> game frame -> sync drops`.
  - `build-line-ring8-wait-noprep`: same RGB565 frame path but ring prep disabled. Observed `blue -> magenta -> sync drops`.
  - `build-direct-ring8-wait-publish-first`: no full RGB565 frame, direct line-ring prep from Doom's indexed framebuffer. Observed `solid blue -> game -> sync drops`.
- Last flashed build before stopping:
  - `build-direct-ring8-wait-publish-first/src/doom_tiny_usb.uf2`
  - Observed: `solid blue -> game -> sync drops`.

### Current Interpretation
The failure is no longer a simple "pico_hdmi pointer callback is bad" issue:
- Solid pointer callback is stable under live Doom.
- Core 0 live Doom rendering by itself is stable when scanout is solid.
- Game-frame scanout is stable when Core 0 is idled.

The failures appear to come from two related pressure points:
- The full 128 KB RGB565 prepared frame reduces SRAM/zone headroom enough to produce glitches or drops under live rendering.
- Direct line-ring preparation avoids that 128 KB buffer, but active per-line preparation still competes with live Doom enough to drop sync.

The most useful refinement is:
**Core 0 render + solid pointer scanout is okay; Core 0 render + live game-line preparation/scanout is not okay yet.**

### Recommended Next Experiments
1. Build a no-prep direct-ring control:
   - `PICODOOM_HDMI_PREPARED_SCANLINES=0`
   - `PICODOOM_HDMI_LINE_RING=1`
   - `PICODOOM_HDMI_LINE_RING_DIRECT=1`
   - `PICODOOM_HDMI_LINE_RING_PREPARE=0`
   - `PICODOOM_PUBLISH_FIRST_FRAME_ONLY=1`
   - `PICODOOM_WAIT_AFTER_FRAME_PUBLISH=1`
   Expected screen is mostly magenta. The question is only whether sync holds. If it holds, direct ring prep is the active contention source. If it drops, the direct-ring pointer state/buffers are still too much.
2. If direct-ring no-prep holds, serialize direct-ring preparation with Core 0 rendering:
   - Stop Core 0 after a publish.
   - Let Core 1 prepare a bounded amount of scanout state.
   - Resume Core 0 only after that work is complete.
   This may reduce FPS but should test whether strict handoff can make real image scanout stable.
3. Revisit 240p or hardware-assisted horizontal repeat:
   - The current 480p path pays for 320->640 expansion in software.
   - A real fix likely needs to avoid active per-line software expansion while Core 0 is rendering.
   - Options worth exploring: updated `pico_hdmi` 240p, HSTX command-expander tricks, or a DMA/control-list approach that repeats pixels without a 640-wide software line.
4. Keep RGB565 full-frame builds as diagnostics, not the main direction, unless a large RAM/zone re-layout is done. The full frame costs 128 KB and has correlated with instability.

### Useful Known Builds
- Stable solid callback while Doom runs:
  `build-solid-live-current/src/doom_tiny_usb.uf2`
- Stable solid pointer callback while Doom runs:
  `build-solid-pointer-live/src/doom_tiny_usb.uf2`
- Stable static game frame when Core 0 idles:
  `build-line-ring8-idlefirst-activeprep/src/doom_tiny_usb.uf2`
- Current actual game baseline with artifacts:
  `build-min/src/doom_tiny_usb.uf2`
- Last failed direct-ring attempt:
  `build-direct-ring8-wait-publish-first/src/doom_tiny_usb.uf2`

### Rebuild Template
```sh
cmake -E rm -rf build-name
cmake -S . -B build-name -G Ninja \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DPICO_SDK_PATH=/Users/dudu/pico-sdk \
  -DPICO_EXTRAS_PATH=/Users/dudu/pico-extras \
  -DPICO_BOARD=pico2 \
  -DPICO_PLATFORM=rp2350-arm-s \
  -DPICO_STDIO_USB=OFF \
  -DPICO_STDIO_UART=ON \
  -DPICODOOM_HDMI_DIAG_STAGE=3 \
  -DPICODOOM_HDMI_DVI_MODE=1 \
  -DPICODOOM_BOOT_TO_E1M1=1 \
  -DPICODOOM_SKIP_WIPES=1 \
  -DPICODOOM_SYS_CLOCK_KHZ=252000 \
  -DPICODOOM_HDMI_HSTX_CLK_DIV=2 \
  -DPICODOOM_RENDER_THROTTLE_US=20
cmake --build build-name --target doom_tiny_usb -j
```

## Session Findings (2026-05-26): HSTX/memory root-cause analysis + FIFO probe

### What the failure is (refined)
- 320 words/line in a ~25 us active line is trivial *bandwidth* (~12 MB/s). The sync drops are **latency/jitter**: the scanout DMA stalls behind Core 0 on a shared SRAM bank, the HSTX FIFO drains, TMDS emits garbage for a beat, and the sink loses PLL lock.
- The differentiator across all prior builds is precise: **scanout that reads frame-derived memory drops sync; scanout of a constant does not.** Static game frame (Core 0 idle) is stable; solid scanout (Core 0 live) is stable; game frame + Core 0 live is not.
- `BUS_PRIORITY` DMA_R is **already set** in two places (`i_video.c:~1283`, `pico_hdmi/src/video_output_rt.c:742`) and it is **not sufficient** — priority resolves per-cycle arbitration but the DMA still stalls behind an in-flight Core 0 transaction on the same bank.

### HSTX hardware doubling: already done, cannot be improved
- `pico_hdmi` runs the command expander in TMDS mode with `EXPAND_SHIFT.ENC_N_SHIFTS=2, ENC_SHIFT=16` (`video_output.c:550`). One FIFO word -> two output pixels.
- `hdmi_expand_native_scanline()` (480p branch, `i_video.c`) writes `out[i] = px | (px<<16)` — 320 words, each pixel duplicated in both halves. So the **2x horizontal repeat is already done in hardware**; the buffer just carries identical halves.
- Setting `ENC_SHIFT=0` would be functionally equivalent (high 16 bits become don't-care) with **no memory/bandwidth/latency win**. The expander rotates by a uniform `ENC_SHIFT` per push, so it cannot turn 160 packed-native words into a correct 640 line; two 16-bit pixels can't pack 4-to-a-word. **2x scale of 16bpp on HSTX is fixed at 320 words/line.** Do not chase HSTX expander reconfiguration for this.

### RP2350 SRAM map (datasheet ch04) + the shortptr constraint
- Banks 0-3 are 4-way word-striped over `0x20000000-0x2003FFFF`; banks 4-7 over `0x20040000-0x2007FFFF`. Each bank has its own AHB arbiter, so accesses to different banks run in parallel. ch14 erratum prescribes splitting DMA vs CPU buffers across these two 256 KB blocks. No non-striped mirror on RP2350.
- The Doom zone uses **shortptrs**: `SHORTPTR_BASE=0x20030000` on RP2350 (`doomtype.h`), window `[0x20030000, 0x20070000)`. `I_ZoneBase` returns `__end__` with size `0x20070000 - __end__`, so the zone is hard-confined to that window (straddles the bank boundary at `0x20040000`).
- From `build-min/.../doom_tiny_usb.elf.map`: `__data_start__=0x20000110`, `__end__=0x200681a4` (~426 KB static data, spanning all 8 banks), `frame_buffer` at `0x20025410` (banks 0-3). Zone is `0x200681a4..0x20070000` (~32 KB). **Only `0x20070000-0x2007FFFF` (64 KB) is free** — the lone RAM region untouched by static data or zone.
- Implication: perfect "Core 0 only touches banks 0-3, DMA only banks 4-7" isolation is impossible (working set is 458 KB). The achievable move is to pin the scanout ring into the free top 64 KB (the quietest region) + keep prep vblank-only. That is a *partial*, measurement-gated experiment, not a guaranteed fix — which is why the FIFO probe comes first.

### FIFO probe (added this session)
- New flag `PICODOOM_HDMI_FIFO_PROBE` (`src/pico/CMakeLists.txt`). When 1: the ISR-context scanline callbacks sample `hstx_fifo_hw->stat` per active line (EMPTY bit 9 = drained = underflow; LEVEL bits 7:0 = margin); `hdmi_fifo_probe_report()` prints `empty=/min_level=/samples=` over UART once per ~60 frames from `pd_end_frame()` (Core 0).
- Covers all three scanout paths (line-ring pointer cb, prepared-RGB565 cb, direct scanline cb). No `pico_hdmi` edit. Verified build: `build-fifoprobe/src/doom_tiny_usb.uf2`, ends `0x1003b054` (safe below WHX `0x10042000`).
- **Next:** flash `build-fifoprobe` and read the UART. `empty>0` during the game image confirms FIFO underflow as the drop cause; then the top-64KB ring relocation can be tried and measured (empty should fall toward 0). If `empty==0` while sync still drops, the cause is elsewhere (e.g. HSTX clock/command-list, not scanout starvation) and the bank work is moot.
- Build/probe command: add `-DPICODOOM_HDMI_LINE_RING=1 -DPICODOOM_HDMI_LINE_RING_DIRECT=1 -DPICODOOM_HDMI_FIFO_PROBE=1` to the rebuild template (UART stdio must stay on).

## Session (2026-06-10): command-list scanout backend (borrowed from fruitjam-doom)

### What was added
`PICODOOM_HDMI_CMDLIST=1` — an alternate HSTX scanout backend in `src/pico/hstx_cmdlist.c`,
adapted from the CircuitPython picodvi RP2350 driver as used by
`~/Projects/references/fruitjam-doom` (`Framebuffer_RP2350.c`, MIT). Architecture:

- The **entire 640x480 frame is one precomputed DMA command list**: a command
  channel writes the pixel channel's al3 registers per slot (write-ring,
  re-triggered by chain), the pixel channel feeds the HSTX FIFO paced by
  `DREQ_HSTX`. **No per-scanline ISR exists** — the only interrupt is one per
  frame (null-trigger terminator) that rewinds the command list. This removes
  the per-line CPU deadline entirely, i.e. the diagnosed latency/jitter failure
  mode of the pico_hdmi path.
- Scanout reads the **native 320x200 RGB565 frame (`hdmi_rgb565_frame`)
  directly**: 16-bit DMA transfers are bus-replicated to `px|(px<<16)` and the
  HSTX expander (same `ENC_N_SHIFTS=2/ENC_SHIFT=16` + RGB565 TMDS rotations as
  pico_hdmi) doubles horizontally; vertical 2x + letterbox is baked into the
  command list as repeated/black row pointers. No 640-wide software expansion
  anywhere.
- The command list + black line (~17 KB) live at `0x20070000` — the free top
  64 KB of SRAM (banks 4-7) identified earlier as the quietest region.
- Core 1: starts the scanout, then free-runs `new_frame_stuff()` once per
  scanout frame (frame handoff + full RGB565 rebuild). A slow rebuild can
  tear; it cannot drop sync.
- Constraints (compile-time enforced): DVI only (no data islands → no HDMI
  audio), `DIAG_STAGE>=3`, 480p, no line ring. pico_hdmi init/runtime is
  skipped entirely (its DMA channels/IRQ are never claimed/enabled).

### Build
`build-cmdlist/` = build-min flags + `-DPICODOOM_HDMI_CMDLIST=1`
(PREPARED_SCANLINES=1, DIAG_STAGE=3, DVI=1, BOOT_TO_E1M1=1, SKIP_WIPES=1,
THROTTLE=20). UF2 end `0x10039bd0` (< WHX `0x10042000`). Map verified: nothing
at `0x2007xxxx`; `__end__=0x2005d858` (~43 KB MORE zone headroom than
build-min's `0x200681a4`, since the line-ring/expanded-line buffers drop out).

### A/B RESULT (2026-06-10): ROOT CAUSE CONFIRMED
**build-cmdlist runs smoothly under live rendering** (user-confirmed on
hardware, normal boot with title/demo loop, no forced E1M1). The pico_hdmi
per-scanline-ISR deadline was the sync-drop cause — NOT bus bandwidth, NOT
per-beat SRAM arbitration. The command-list scanout (no per-line CPU
involvement) holds sync where every pico_hdmi variant dropped.

Consequences:
- **Command-list scanout is the way forward.** The pico_hdmi ISR path and its
  diagnostic ladder (line ring, prepared scanlines, FIFO probe, bank
  isolation) are superseded for video.
- **Core 1 render assist is plausibly re-enableable** (`USE_CORE1_*` in
  `pd_render.cpp`): no ISR shares Core 1 anymore; its only duty is the
  once-per-frame `new_frame_stuff()` rebuild.
- **Open problem: audio.** cmdlist is DVI-only. Options: graft data-island
  insertion into the command list (audio packets land in blanking-line
  commands rewritten per frame), or follow fruitjam-doom and use an external
  I2S DAC.
- Next polish candidates: full-screen stretch (row*200/480, drop letterbox),
  tear avoidance (double-buffer the RGB565 frame or rebuild paced to the
  per-frame IRQ phase).

## HDMI audio attempt over cmdlist (2026-06-10, UNRESOLVED -> pivot)

Goal: HDMI audio without giving up the command-list scanout. Evidence trail:

1. HDMI-mode cmdlist (islands in idle region after hsync, multi-island vblank
   lines): RT4K decoded AVI (VIC1/RGB) + audio infoframe (48kHz 2ch LPCM) --
   islands/BCH/TERC4/encoder all work over the cmdlist transport. Audio
   bunched into vblank (200 packets in 1.4ms, 15ms gap) -> silent (sink FIFO
   burst/starve; real sources always spread packets).
2. Rewrote: one island per ACTIVE line inside the hsync pulse (pico_hdmi's
   placement), 96-line ring refilled ahead of beam, ACR every 48 lines,
   AVI/AIF on two vblank lines. Test tone injected at the encoder. Silent.
3. Added proper IEC60958 channel status (hstx_packet_set_audio_samples_cs in
   pico_hdmi, 48kHz consumer L-PCM + parity over VUC). Silent on RT4K AND
   direct TV.
4. **pico_hdmi bouncing-box demo plays music fine on the same hardware/sink**
   -> encoder + sink + in-pulse placement proven good via pico_hdmi's
   ping/pong ISR transport.
5. Host-side diff (/tmp/hstx_line_diff.c): my active-line-with-island buffer
   is word-for-word IDENTICAL to pico_hdmi's build_line_with_di output (56
   words, same null island). Content equality proven.
6. Static-tone build (PICODOOM_HDMI_AUDIO_TEST_TONE=2: schedule baked into
   the ring at init, ZERO dynamic writes): still silent.

Conclusion: identical line content played through the cmdlist DMA transport
is silent while pico_hdmi's ISR transport plays -- the differentiator is
unidentified (suspects exhausted: content, dynamics, channel status, burst
pacing, sink strictness). NOTE: infoframe decode was only ever confirmed for
the idle-region placement (step 1); never re-confirmed for the in-pulse
builds, so possibly NO island decodes in-pulse via cmdlist -- unexplained
since content is identical.

### Further evidence (same day)
- ALL standalone pico_hdmi demos play audio: bouncing_box 480p (islands
  IN-PULSE, sync 96px), directvideo_240p (in-pulse, sync 192px),
  bouncing_box_rt 720p (back-porch idle region). So in-pulse islands at 480p
  ARE accepted by the sink -- via pico_hdmi's ping/pong transport.
- `build-picohdmi-audio/` (pico_hdmi transport inside the Doom build:
  HDMI mode, solid-pointer video, tone pumped into hstx_di_queue from
  core1_background_task, 252 MHz / div 2): **TONE PLAYS** -- audio proven in
  our environment/clocking/lib version -- **but sync drops after ~1 s**:
  pico_hdmi's HDMI mode runs build_line_with_di in the scanline ISR, which
  re-creates the ISR-deadline failure under live Doom.
- Net: cmdlist = stable video + mute audio; pico_hdmi ISR = working audio +
  dropped sync. The cmdlist audio mystery remains unexplained (identical
  line content, same clocking, same encode) -- suspicion now narrows to
  DMA chain-gap timing at slot boundaries vs pico_hdmi's contiguous
  whole-line transfers, but unverified.

### Decision: pivot to "pico_hdmi-lite" (user-proposed)
Keep pico_hdmi's per-line ISR runtime (the exact path the demo proves works
for audio), but strip the ISR of what made it drop sync -- rendering. The
diagnostics history shows pointer-only ISR was stable under live Doom
(build-solid-pointer-live). Plan:
- B1: pico_hdmi runtime at 252MHz/div2, solid-color pointer callback, audio
  pump (encode 4-sample islands from i_picosound ring -> hstx_di_queue).
  Expect: stable video + test tone. Proves audio in our integration.
- B2: pointer callback returns native 320px RGB565 frame rows; pixel-data DMA
  switched to 16-bit (bus replication, like cmdlist) so the ISR does pointer
  arithmetic only -- no 640-wide expansion, no rendering in ISR.
- The audio ring/mixer in i_picosound.c (this session) carries over as-is.
- cmdlist backend stays as the proven DVI/video-only fallback
  (PICODOOM_HDMI_CMDLIST=1 + DVI_MODE=1).

## Plan (2026-06-11): integration ladder to "flawless"

### State assessment
The LITE pivot is fully implemented in the working tree (uncommitted):
- `pico_hdmi`: `PICO_HDMI_PRECOMPOSED_ACTIVE_LINES` compose ring (ISR =
  pointer lookup + tag check, stale entry -> static null island), native
  16-bit pixel DMA mode (per-post ctrl swap, resync-safe),
  `hstx_packet_set_audio_samples_cs`, `video_output_in_vertical_blanking`,
  optional scratch-Y line buffer.
- Parent: `PICODOOM_HDMI_LITE=1` wiring; 48 kHz mixer ring in
  `i_picosound.c` (SFX + music, `I_PicoSoundPullStereo` consumer API);
  hand-managed 0x20070000 region layout (compose ring 0x20070000, audio
  ring 0x20077800, diag canvas 0x20079800, status buffer 0x2007D400);
  on-screen diag overlay + checkpoint breadcrumbs; USB-CDC diag plumbing
  (`PICODOOM_CDC_WAIT`, tusb device mode for non-USB targets, BOOTSEL
  backdoors via UART 0x02+'B' and Ctrl+Alt+Del).
- Last builds: `build-lite` (LITE, doom_tiny_usb), `build-diag` (LITE,
  doom_tiny + CDC), `build-diag2` (cmdlist DVI control, doom_tiny + CDC).
  **Hardware outcomes of these were not recorded** — re-establish ground
  truth at the next bench session and log it here.

### Architecture decision (locked)
**Primary: LITE.** Rationale: the per-line ISR workload is now <= the
proven-stable solid-pointer build (pointer swap only); the DMA reads of
`hdmi_rgb565_frame` are proven harmless by cmdlist stability; the audio
rides the exact ping/pong transport that `build-picohdmi-audio` and all
pico_hdmi demos prove works on this hardware/clocking.
**Fallback (video):** cmdlist DVI (`build-cmdlist`) — always-good baseline.
**Fallback (audio), only if LITE fails a gate unrecoverably:** cmdlist DVI
video + external I2S DAC (fruitjam-doom approach); the mixer ring carries
over unchanged. Do NOT resume the cmdlist-HDMI silence mystery.

### Pre-gate fixes (code, before any flashing)
1. **Compose-ring starvation (likely the LITE audio-breaker).** Ring lead is
   96-8 = 88 lines ~= 2.8 ms, but Core 1's per-frame RGB565 rebuild (~3 ms)
   runs every frame WITHOUT servicing the ring -> stale entries -> null-island
   fallback -> already-dequeued audio packets silently dropped every frame.
   Fix: interleave `video_output_compose_service()` (+ audio pump) into the
   rebuild loop every ~16 rows. Also add an ISR counter for stale-ring
   fallbacks ("ST" on the overlay) so this failure mode is *visible*.
2. **Region-layout asserts.** The four magic 0x2007xxxx addresses live in two
   files; move them to one header with static asserts (sizes, no overlap,
   fits 64 KB) so a future buffer resize can't silently collide.
3. Commit the working tree (parent branch + nested pico_hdmi) as the
   baseline before gate testing; every gate result gets logged here.

### Gate ladder (one variable per gate; binary observable; log result)
- **G1 control:** reflash `build-cmdlist` (DVI). Expect: smooth game, mute.
  Confirms environment unchanged.
- **G2 transport at rest:** LITE + `AUDIO_TEST_TONE=1` +
  `IDLE_AFTER_FIRST_DISPLAY=1`. Expect: static game frame + clean continuous
  1 kHz tone, fe=0, ST=0. Proves compose ring + native 16-bit DMA + in-pulse
  islands without Core 0 load.
- **G3 transport under load:** LITE + `AUDIO_TEST_TONE=1`, live game.
  Expect: smooth gameplay + uninterrupted tone >= 10 min, fe=0, ST=0.
  Proves the per-line deadline holds under full Core 0 contention.
- **G4 game SFX:** LITE, tone off, music generator left NULL. Expect: menu
  pistol/door sounds, MX/PL counters advancing together.
- **G5 music:** enable OPL music. Expect: E1M1 music + SFX mixed; cp walks
  31->34 on level change and never hits 99 (zone OOM); print `Z_FreeMemory`
  after `S_ChangeMusic` once.
- **G6 soak + edges:** 30-60 min attract/demo loop; level transitions;
  save+load during play (flash writes vs scanout); pause/menu; wipes back on
  (`SKIP_WIPES=0`); throttle removed (`RENDER_THROTTLE_US=0`); both sinks
  (RT4K and TV direct).
- **G7 productionize:** diag overlay/CDC/backdoors default-off;
  `doom_tiny_usb` (USB-host keyboard, UART stdio) as shipping target;
  `flash.sh`/`build-min` repointed at the final config; docs + memory
  updated; commit; keep the final build dir as the new known-good.

### Contingencies
- G2 silent: bisect compose ring vs native DMA (force legacy in-ISR compose
  with Core 0 idle — known to play — and diff); check
  `samples_per_line_fp` pacing and `hstx_di_queue_tick` semantics.
- G3 drops sync: fe>0 = FIFO starvation -> try
  `PICO_HDMI_LINE_BUFFER_IN_SCRATCH_Y=1`, larger ring lead; if unfixable,
  fall back to cmdlist DVI + I2S DAC.
- G3 tone gaps with ST>0: rebuild chunking too coarse / ring too small —
  shrink chunk, grow ring (region has ~1 KB spare; steal from diag canvas).
- G5 zone OOM (cp=99): measure first; reclaim by disabling diag buffers in
  production; note rp2040-doom shipped sound+music in 264 KB total, so the
  ~75 KB zone should suffice.

### Session 2026-06-11 (later): pre-gate fixes done, gate builds ready
- Pre-gate fixes implemented and committed (parent `89aa021b`, pico_hdmi
  `5504c0b`):
  - Compose-ring starvation fix: `hdmi_rgb565_build_display_frame()` calls
    `video_output_compose_service()` + `hdmi_audio_pump()` every 16 rows.
  - LITE no longer clears `hdmi_rgb565_frame_ready` per rebuild (that would
    black out ~95 lines every frame; direct-scan modes tear instead).
  - Stale-fallback counter `video_output_precomposed_stale_count`: shown as
    `ST` on diag overlay row 3 and in the once-per-second `LITE ...` UART
    report. Small startup burst normal; growth while running = compose
    starvation = dropped audio packets.
  - `src/pico/hdmi_lite_layout.h`: single source of truth + static asserts
    for the 0x20070000 region (compose ring / audio ring / text canvas /
    status buffer).
- Gate builds (all doom_tiny_usb, UF2 end < 0x10042000, nothing linked at
  0x2007xxxx, `__end__=0x200674d0` -> ~35.6 KB zone):
  - **G1**: `build-cmdlist/` (rebuilt against current tree, end 0x10039cb8)
  - **G2**: `build-lite-g2/` (LITE + TONE=1 + IDLE_AFTER_FIRST_DISPLAY=1,
    end 0x1003fd24) — **FLASHED to the board 2026-06-11**
  - **G3**: `build-lite-g3/` (LITE + TONE=1, live game, end 0x1003fd04)
- G2 expected observation: HDMI mode, title-screen frame frozen (Core 0
  idles after first display), continuous clean 1 kHz square tone, overlay
  rows in the top letterbox with FE flat, ST flat after boot. Record the
  result here, then flash `build-lite-g3` (`pi flash`, WHX already present).
- Note: G2/G3 UF2s end ~9 KB below the WHX base — watch this margin when
  adding code to LITE builds.

### G2 result #1 (2026-06-11) + ROOT CAUSE: vpatchlists collision
- Observed: title + tone fine; at attract-mode entry, black screen then
  **signal drop** (= Core 1 death). Note `IDLE_AFTER_FIRST_DISPLAY` only
  engages at `gamestate==GS_LEVEL`, so G2 runs the full demo-entry path
  live before idling — the gate legitimately exercised the transition.
- Root cause (found by inspection, fixed in `69ec8fb4`): **the top 64 KB is
  NOT free.** `pd_render.cpp` hand-places `vpatchlists` (3 KB) at
  `SRAM_SCRATCH_X_BASE - 0xc00 = 0x2007F400` on RP2350 — a cast pointer,
  invisible in the linker map. LITE's `hdmi_status_buffer`
  (0x2007D400+0x2800) overlapped it by 2 KB. First status-bar render (demo
  entry) trampled the overlay lists Core 1 walks in
  `new_frame_init_overlays...` (asserts compiled out in MinSizeRel) → wild
  writes into SCRATCH_X (pico_hdmi ISR data) → signal drop. Title was fine
  because the status buffer is untouched until a level HUD renders. Also
  explains build-lite's undocumented demo-entry deaths and why cmdlist
  (status buffer in BSS) plays the whole demo loop.
- Fix: LITE region repacked below `0x2007F400` (compose ring 96→88), the
  squatter documented + asserted in `hdmi_lite_layout.h` and capped in
  `hstx_cmdlist.c`. **Usable region is 0x20070000–0x2007F400 (61 KB).**
- Fixed G2 reflashed 2026-06-11. Expected now: title + tone → demo entry →
  first level frame frozen + tone continues + signal holds. Then G3.

### G2/G3 PASS (2026-06-11, post-vpatchlists-fix)
- **G2 PASS**: title + tone → demo entry → first level frame frozen, tone
  continuous, signal held.
- **G3 PASS**: live attract loop, smooth video + uninterrupted tone, 5+ min
  ("going strong"). Transport fully proven under live Core 0 load.
- **G4 PASS**: real SFX through the mixer ring (no OPL), audio and video
  stable.
- **G5 PASS (2026-06-11): full game — live video + SFX + OPL music over
  HDMI, stable.** The LITE architecture is validated end-to-end. Remaining:
  G6 soak (long run, save/load, wipes on, throttle off, second sink) and G7
  productionize (diag overlay off, repoint flash.sh/build-min).
- Released as `v0.1.0-hdmi-audio` (pre-release; G6/G7 pending), built from
  `build-lite-g5`. pico_hdmi pinned at `5504c0b`
  (branch `doom-hdmi-lite` on fliperama86/pico_hdmi).
- Remaining pre-known risks for G3+: concurrent Z_Malloc (Core 1's first
  PLAYPAL `W_CacheLumpNum` vs Core 0 zone churn — only the libc wrappers
  hard_assert, `Z_Malloc` itself has no cross-core lock), zone headroom
  (~35.6 KB with sound enabled; cp=99 breadcrumb), Core 1 stack (8 KB, now
  also carries printf/snprintf/compose).

### Audio distortion hunt (2026-06-11, post-release) — RESOLVED
E1M1 music sounded metallic/distorted. The pure-sine discriminator
(`PICODOOM_HDMI_AUDIO_TEST_TONE=3`, 750 Hz) + spectral analysis of phone
recordings (`ffmpeg` + scipy: welch/periodogram/spectrogram) isolated and
fixed THREE stacked transport defects (per-packet encoding was audited
clean on host — /tmp/cs_audit.c):
1. **Silence packet asserted the IEC block-start flag** (encoded with
   frame_count=0): every underrun insertion reset the sink's channel-status
   sync. Fixed (mid-block frame_count) + counted
   (`hstx_di_queue_silence_count`, SL on overlay).
2. **Ahead-of-beam compose starved by Core 1 background stalls**: every
   per-frame section longer than the ring's 2.5 ms lead (frame init,
   rebuild tail, canvas redraw, and a 7 ms once-per-second printf = an
   audible 1 Hz bip) staled entries and dropped their packets — measured as
   ±60 Hz sidebands around a perfect 750 Hz carrier, ST ~7 lines/frame.
   Fixed ARCHITECTURALLY: headers are static and built once; the scanline
   ISR pops pre-encoded islands from the di queue and patches 36 words into
   the line being posted (~1.5 µs). Audio pacing lives in the ISR; the
   background task can no longer starve it. All runtime serial diagnostics
   removed (overlay only).
3. **Active-lines-only delivery**: pacing audio over the 480 active lines
   left a 45-line (1.4 ms) hole every vblank — 60 Hz delivery duty cycle
   the sink resamples around (Morph4K SF CS flipped 48/44.1; carrier
   wandered 644-785 Hz). Fixed: per-line tick restored for all 525 lines,
   islands patched into ping/pong blanking templates too — identical
   delivery shape to the demos that play cleanly.
**Result: user-confirmed CLEAN sine.** Lesson: square-wave test tones mask
dropped/repeated-sample artifacts — always validate audio transports with
a pure sine + spectrum analysis of a recording.

### USB keyboard (2026-06-11) — RESOLVED
Two stacked causes:
1. Software: `CFG_TUH_DEVICE_MAX 1` (hub ate the only device slot), 1 hub
   slot, 128-byte enumeration buffer. Fixed in `38e0dd21` (4 devices, 2
   hubs, 256 bytes).
2. Hardware: the board is a **WeAct RP2350B on a custom PCB powered via
   VSYS** — the USB connector's VBUS is behind the VBUS→VSYS diode, so the
   connector pin stayed at 0 V: a direct keyboard got no power, and the
   powered hub kept its downstream ports off (hubs gate them on upstream
   VBUS presence). Fix: strap the 5 V rail to the board's 5V/VBUS pin.
   CAUTION: with VBUS driven, never connect the Mac's USB cable while the
   PSU is on (no diode between them) — use a jumper on the strap.
Confirmed working on hardware (overlay KB M/H/R counters + gameplay).

KNOWN REMAINING (separate, mild): emu8950 with EMU8950_NO_RATECONV ignores
the requested rate and outputs chip-native 49716 Hz; played at 48 kHz the
music is ~3.45% flat/slow. Fix candidates: feed OPL_calc through a simple
49716→48000 fractional resampler in the mixer, or pace AdvanceTime in chip
samples. Do AFTER confirming music is otherwise clean.

### Post-flawless polish backlog (do not mix into the gates)
Tear: rebuild already starts at frame IRQ; vblank+letterbox (~2.9 ms) nearly
covers the ~3 ms rebuild — fine-tune pacing only if visible. Full-screen
stretch (`row*200/480`). Core 1 render assist: evaluate FPS first; it
competes with compose deadlines under LITE, so only revisit if needed.
cmdlist HDMI-audio mystery: parked indefinitely.
