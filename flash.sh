#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
BUILD_DIR="${ROOT}/build"
WHX_FILE="${ROOT}/doom1.whx"
WHX_ADDR="0x10042000"

if [[ $# -gt 1 ]]; then
    echo "usage: $0 [path/to/doom_tiny_usb.uf2]" >&2
    exit 2
fi

if [[ $# -eq 1 ]]; then
    UF2_FILE="$1"
else
    # release configuration (v0.1.2 lineage): LITE HDMI+audio, C OPL
    # renderer, no diag overlay, full speed, WHX at 0x10080000
    cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=MinSizeRel \
        -DPICO_BOARD=pico2 \
        -DPICO_PLATFORM=rp2350-arm-s \
        -DPICO_STDIO_USB=OFF -DPICO_STDIO_UART=ON \
        -DPICODOOM_HDMI_DIAG_STAGE=3 \
        -DPICODOOM_HDMI_DVI_MODE=0 \
        -DPICODOOM_HDMI_LITE=1 \
        -DPICODOOM_EMU8950_ASM=0 \
        -DPICODOOM_DIAG_OVERLAY=0 \
        -DPICODOOM_DOOM_TINY_USB_WAD_ADDR=0x10080000 \
        -DPICODOOM_SKIP_WIPES=1 \
        -DPICODOOM_SYS_CLOCK_KHZ=252000 \
        -DPICODOOM_HDMI_HSTX_CLK_DIV=2 \
        -DPICODOOM_RENDER_THROTTLE_US=0
    cmake --build "$BUILD_DIR" -j --target doom_tiny_usb
    UF2_FILE="${BUILD_DIR}/src/doom_tiny_usb.uf2"
fi

if [[ ! -f "$UF2_FILE" ]]; then
    echo "missing UF2: $UF2_FILE" >&2
    exit 1
fi

if [[ ! -f "$WHX_FILE" ]]; then
    echo "missing WHX payload: $WHX_FILE" >&2
    exit 1
fi

# The UF2 advertises its WHX load address via binary_info ("WHX at 0x...");
# trust the binary over the hardcoded default so relocated builds (e.g.
# 0x10080000 on 16 MB boards) flash correctly.
DETECTED_ADDR=$(picotool info "$UF2_FILE" 2>/dev/null | sed -n 's/.*WHX at \(0x[0-9a-fA-F]*\).*/\1/p' | head -1 || true)
if [[ -n "${DETECTED_ADDR}" ]]; then
    WHX_ADDR="$DETECTED_ADDR"
fi
echo "WHX address: $WHX_ADDR"

picotool load -v -F "$UF2_FILE"
picotool load -v -t bin "$WHX_FILE" -o "$WHX_ADDR"
picotool reboot -a
