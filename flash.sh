#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
BUILD_DIR="${ROOT}/build-min"

# keep the stage consistent with our latest diagnostics
cmake -S "$ROOT" -B "$BUILD_DIR" \
    -DPICODOOM_HDMI_DIAG_STAGE=3 \
    -DPICODOOM_HDMI_DVI_MODE=1 \
    -DPICODOOM_SOLID_COLOR=0x8410
cmake --build "$BUILD_DIR" -j8 --target doom_tiny_usb

picotool load -v -f "${BUILD_DIR}/src/doom_tiny_usb.uf2"
picotool load -v -t bin doom1.whx -o 0x10042000
picotool reboot -a
