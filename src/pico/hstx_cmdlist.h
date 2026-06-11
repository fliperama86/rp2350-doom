#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Command-list HSTX scanout backend (PICODOOM_HDMI_CMDLIST=1).
// Starts 640x480@60 DVI scanout of a native 320-wide RGB565 frame
// (pitch_words 32-bit words per row, frame_lines rows, letterboxed and
// 2x-scaled to 640x400 inside 480 lines). Call from Core 1; the per-frame
// DMA IRQ is taken on the calling core. Does not return until scanout runs.
void hstx_cmdlist_scanout_start(const uint32_t *frame_base, uint32_t pitch_words,
                                uint32_t frame_lines);

// Increments once per scanned-out frame (in the per-frame DMA IRQ).
uint32_t hstx_cmdlist_frame_number(void);

// HDMI mode only (PICODOOM_HDMI_DVI_MODE=0): audio packets ride one data
// island per active line, fed from a ring refilled ahead of the beam.
// Call frame_begin once per frame right after the frame IRQ (prefills the
// ring while the beam is in blanking), then poll frequently from Core 1's
// idle loop to keep filling ahead of the scan. No-ops in DVI mode.
void hstx_cmdlist_audio_frame_begin(void);
void hstx_cmdlist_audio_poll(void);

#ifdef __cplusplus
}
#endif
