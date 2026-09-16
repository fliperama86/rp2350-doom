# Attract-mode crash diagnostics

This is instrumentation, not a proposed crash fix. `PICODOOM_CRASH_DIAG=1`
requires RP2350 ARM Secure, LITE scanout, and `PICODOOM_DIAG_OVERLAY=1`.
Clocks, HDMI mode, audio, and the attract sequence are unchanged.

## Build and package

The September 2026 board has 2 MB flash. Release and diagnostic builds now
use WHX at `0x10044000`. `flash.sh` explicitly disables both diagnostics on
release builds, even when reusing a diagnostic cache; enable them as below.

Starting from the existing release-configured canonical `build/`:

```sh
cmake -S . -B build -DPICODOOM_CRASH_DIAG=1 -DPICODOOM_DIAG_OVERLAY=1 \
  -DPICODOOM_DOOM_TINY_USB_WAD_ADDR=0x10044000
cmake --build build --target doom_tiny_usb -j 8
uv run --with unicorn --with pyelftools src/pico/crash_diag/test_capture.py \
  build/src/doom_tiny_usb.elf
python3 src/pico/crash_diag/package_uf2.py build/src/doom_tiny_usb.uf2 \
  doom1.whx build/diagnostics/doom_crash_full.uf2
```

Prefer **doom_crash_full.uf2** when the board's game-data placement is unknown.
Boards predating this diagnostic used WHX at `0x10042000`; a code-only update
from that layout is insufficient. The packager checks firmware/BIN agreement, overlap,
erase-sector alignment, the 2 MB limit, and every output payload byte. It uses
picotool to retain the RP2350-E10 absolute-block workaround.

Keep the exact ELF with each tested UF2. The pre-diagnostic reproducer is in
`build/diagnostics/baseline-20260915/`, including its ELF and build cache.
The same packager can make a full rollback image from that baseline UF2.

## Readout

During normal operation:

- LP: Doom loop count; TC: game tick; CP: existing transition checkpoint.
- FZ: last sampled zone free bytes; HC: heap validation result; C1: HDMI frames.
- DM: attract sequence index; PB/CN: published/consumed game frames; RS: HDMI resyncs.

If the picture freezes, photograph all three lines. If no fatal report appears,
check whether C1 still advances. This helps distinguish a game-side stall from
an HDMI-side stall, but does not locate an arbitrary deadlock.

A captured fatal stop replaces these rows with `C0 K... PC...`:

| K | Meaning | D | X |
|---|---|---|---|
| 1 | CPU fault | Fault-address registers and exception return shown instead | |
| 2 | Engine `I_Error` | Source line | 0 |
| 3 | Out of zone memory | Requested allocation size including header/alignment | Allocation tag |
| 4 | SDK panic | 0 | Format-string address |
| 5 | Assertion | Source line | File-string address |
| 6 | Heap corruption detected | `Z_ValidateHeap` result | 0 |

FS/HS are CFSR/HFSR; SP is the stopped stack pointer. CPU faults additionally
show BF/MM (BFAR/MMFAR), EX (EXC_RETURN), and V (stacked frame validity).
BFAR/MMFAR are only meaningful when their validity bits in CFSR are set.
V=0 means no safe frame decode was attempted, not that PC really was zero.
Hardware-fault PC is the stacked instruction address; for software stops PC
is the caller return address, so resolve `(PC & ~1) - 1` with addr2line:

```sh
arm-none-eabi-addr2line -e build/src/doom_tiny_usb.elf -f -C 0xADDRESS
```

The handler and literal pool execute from RAM, without stack accesses except
validated frame reads. It does not print, allocate, reset the board, change
clocks, or mask interrupts on the other core. Records are per-core and published
last after a barrier. Panic/assert wrappers avoid deadlocking inside stdio.
Heap validation runs before the pre-existing unbounded free-memory walk.

Frame layout reference: [Arm's Cortex-M33 fault example](https://github.com/ARM-software/CMSIS-RTX/blob/main/Examples/TrustZoneV8M/RTOS_Faults/CM33_s/Hardfault.c).
Only Secure/default-stacked frames in physical SRAM are decoded. Stack-entry
errors suppress frame reads. Non-secure/security-transition frames are not
supported by this firmware's decoder.

## Limits and verification

The on-screen report depends on Core 1 and HDMI still running. A Core 1 fault,
a fault while holding a lock Core 1 needs, a global XIP failure, or lost power
can prevent the readout. Records then require SWD and are lost on reset.
Faults before RAM initialization/HDMI startup cannot yield an on-screen report.
No claim that a missing report rules out a fault.

`test_capture.py` runs the **linked ARM machine code** with mocked SRAM/MMIO.
It checks both cores, software errors, wrappers, MSP/PSP and FP/basic frames,
invalid stack bounds/stacking failures, publication ordering, vector-table
overrides, and readout width/glyph coverage. This is not hardware validation
of exception entry, real bus faults, or HDMI behavior. The hardware reproduction
is still required.
