//
// Single source of truth for the hand-placed buffers in the top 64 KB of
// SRAM (0x20070000-0x2007FFFF, banks 4-7) used by the LITE backend. The
// linker never places anything in this region, so it is the quietest RAM
// for everything the scanline ISR and its DMA touch per line. The cmdlist
// backend parks its scanout_ram_t at the same base; the two backends are
// compile-time mutually exclusive, so they can never coexist.
//
// CAUTION: the top 3 KB (0x2007F400-0x20080000) is NOT free. pd_render.cpp
// hand-places `vpatchlists` at SRAM_SCRATCH_X_BASE - 0xc00 on RP2350 -- a
// cast pointer, invisible in the linker map. Core 0 rewrites it every frame
// (overlay lists) and Core 1 walks it in new_frame_init_overlays...().
// The original LITE layout overlapped it by 2 KB: the moment the status bar
// first rendered (demo entry), the overlay lists were trampled, Core 1
// chased garbage list indexes into SCRATCH_X and the HDMI signal dropped.
// Everything here must stay below HDMI_LITE_REGION_END.
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
#define HDMI_LITE_REGION_END  0x2007f400u // vpatchlists owns 0x2007f400+

// Pre-composed active-line headers (video_output_precomposed_line_t[]),
// consumed by pico_hdmi's scanline ISR. 88 entries = 80-line lead; the
// per-frame RGB565 rebuild services the ring mid-loop, so it never stales.
#define HDMI_LITE_COMPOSE_RING_ADDR    0x20070000u
#define HDMI_LITE_COMPOSE_RING_ENTRIES 88u

// Mixed stereo sample ring (i_picosound.c), drained into audio data islands.
#define HDMI_LITE_AUDIO_RING_ADDR  0x20076c00u
#define HDMI_LITE_AUDIO_RING_BYTES 0x2000u

// On-screen diagnostic text canvas (RGB565 rows, i_video.c).
#define HDMI_LITE_TEXT_CANVAS_ADDR  0x20078c00u
#define HDMI_LITE_TEXT_CANVAS_BYTES 0x3c00u

// hdmi_status_buffer (indexed status-bar overlay scratch, i_video.c).
#define HDMI_LITE_STATUS_BUF_ADDR  0x2007c800u
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
               "status buffer overflows into vpatchlists");
