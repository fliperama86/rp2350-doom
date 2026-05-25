#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
BUILD_DIR="${ROOT}/build-min"

# keep the stage consistent with our latest diagnostics
cmake -S "$ROOT" -B "$BUILD_DIR" \
    -DPICODOOM_HDMI_DIAG_STAGE=3 \
    -DPICODOOM_HDMI_DVI_MODE=1 \
    -DPICODOOM_HDMI_240P=0 \
    -DPICODOOM_HDMI_PREPARED_SCANLINES=1 \
    -DPICODOOM_SKIP_WIPES=1 \
    -DPICODOOM_BOOT_TO_E1M1=1 \
    -DPICODOOM_FORCE_SOLID_SCANOUT=0 \
    -DPICODOOM_PUBLISH_FIRST_FRAME_ONLY=0 \
    -DPICODOOM_IDLE_AFTER_FIRST_DISPLAY=0 \
    -DPICODOOM_FREEZE_POINT=0 \
    -DPICODOOM_RENDER_THROTTLE_US=20 \
    -DPICODOOM_FRAME_CAP_FPS=0 \
    -DPICODOOM_SOLID_COLOR=0
cmake --build "$BUILD_DIR" -j8 --target doom_tiny_usb

picotool load -v -f "${BUILD_DIR}/src/doom_tiny_usb.uf2"
picotool load -v -t bin doom1.whx -o 0x10042000
picotool reboot -a
