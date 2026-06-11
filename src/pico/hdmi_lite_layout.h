//
// Single source of truth for the hand-placed buffers in the top 64 KB of
// SRAM (0x20070000-0x2007FFFF, banks 4-7) used by the LITE backend. The
// linker never places anything in this region (the build would already have
// failed on size long before), so it is the quietest RAM for everything the
// scanline ISR and its DMA touch per line. The cmdlist backend parks its
// scanout_ram_t at the same base; the two backends are compile-time mutually
// exclusive, so they can never coexist.
//
// Per-buffer capacity asserts live at each buffer's point of use (where the
// element types are visible); this header asserts ordering and region fit.
//
#pragma once

#if !PICODOOM_HDMI_LITE
#error hdmi_lite_layout.h is only meaningful in PICODOOM_HDMI_LITE builds
#endif

#include "pico_hdmi/video_output.h"

#define HDMI_LITE_REGION_BASE 0x20070000u
#define HDMI_LITE_REGION_END  0x20080000u

// Pre-composed active-line headers (video_output_precomposed_line_t[]),
// consumed by pico_hdmi's scanline ISR.
#define HDMI_LITE_COMPOSE_RING_ADDR    0x20070000u
#define HDMI_LITE_COMPOSE_RING_ENTRIES 96u

// Mixed stereo sample ring (i_picosound.c), drained into audio data islands.
#define HDMI_LITE_AUDIO_RING_ADDR  0x20077800u
#define HDMI_LITE_AUDIO_RING_BYTES 0x2000u

// On-screen diagnostic text canvas (RGB565 rows, i_video.c).
#define HDMI_LITE_TEXT_CANVAS_ADDR  0x20079800u
#define HDMI_LITE_TEXT_CANVAS_BYTES 0x3c00u

// hdmi_status_buffer (indexed status-bar overlay scratch, i_video.c).
#define HDMI_LITE_STATUS_BUF_ADDR  0x2007d400u
#define HDMI_LITE_STATUS_BUF_BYTES 0x2800u

_Static_assert(HDMI_LITE_COMPOSE_RING_ADDR == HDMI_LITE_REGION_BASE,
               "compose ring must start the region");
_Static_assert(HDMI_LITE_COMPOSE_RING_ENTRIES * sizeof(video_output_precomposed_line_t) <=
                   HDMI_LITE_AUDIO_RING_ADDR - HDMI_LITE_COMPOSE_RING_ADDR,
               "compose ring overlaps the audio ring");
_Static_assert(HDMI_LITE_AUDIO_RING_ADDR + HDMI_LITE_AUDIO_RING_BYTES <= HDMI_LITE_TEXT_CANVAS_ADDR,
               "audio ring overlaps the text canvas");
_Static_assert(HDMI_LITE_TEXT_CANVAS_ADDR + HDMI_LITE_TEXT_CANVAS_BYTES <= HDMI_LITE_STATUS_BUF_ADDR,
               "text canvas overlaps the status buffer");
_Static_assert(HDMI_LITE_STATUS_BUF_ADDR + HDMI_LITE_STATUS_BUF_BYTES <= HDMI_LITE_REGION_END,
               "status buffer overflows the 64 KB region");
