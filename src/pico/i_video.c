//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
// Copyright(C) 2021-2022 Graham Sanderson
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	DOOM graphics stuff for Pico — HDMI output via pico_hdmi.
//

#if PICODOOM_RENDER_NEWHOPE
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <doom/r_data.h>
#include "doom/f_wipe.h"
#include "pico.h"

#include "config.h"
#include "d_loop.h"
#include "deh_str.h"
#include "doomtype.h"
#include "i_input.h"
#include "i_joystick.h"
#include "i_system.h"
#include "i_timer.h"
#include "i_video.h"
#include "m_argv.h"
#include "m_config.h"
#include "m_misc.h"
#include "tables.h"
#include "v_diskicon.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"

#include "pico_hdmi/video_output.h"
#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico/multicore.h"
#include "pico/sync.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "picodoom.h"
#include "image_decoder.h"
#if PICO_ON_DEVICE
#include "hardware/dma.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/structs/busctrl.h"

#ifndef PICODOOM_HDMI_FIFO_PROBE
#define PICODOOM_HDMI_FIFO_PROBE 0
#endif
#if PICODOOM_HDMI_FIFO_PROBE || PICODOOM_HDMI_LITE
#include "hardware/structs/hstx_fifo.h"
#endif
#endif

// RGB565 pixel macro (replaces PICO_SCANVIDEO_PIXEL_FROM_RGB8)
#define PIXEL_FROM_RGB8(r, g, b) ((((r) >> 3u) << 11u) | (((g) >> 2u) << 5u) | ((b) >> 3u))

#define SUPPORT_TEXT 0

#define USE_INTERP PICO_ON_DEVICE
#if USE_INTERP
#include "hardware/interp.h"
#endif

CU_REGISTER_DEBUG_PINS(scanline_copy)
//CU_SELECT_DEBUG_PINS(scanline_copy)

static const patch_t *stbar;

volatile uint8_t interp_in_use;

#ifndef PICODOOM_HDMI_DIAG_STAGE
#define PICODOOM_HDMI_DIAG_STAGE 0
#endif

#ifndef PICODOOM_HDMI_DVI_MODE
#define PICODOOM_HDMI_DVI_MODE 1
#endif

#ifndef PICODOOM_HDMI_240P
#define PICODOOM_HDMI_240P 0
#endif

#ifndef PICODOOM_HDMI_PREPARED_SCANLINES
#define PICODOOM_HDMI_PREPARED_SCANLINES 0
#endif

#ifndef PICODOOM_HDMI_FREEZE_RGB565_FRAME
#define PICODOOM_HDMI_FREEZE_RGB565_FRAME 0
#endif

#ifndef PICODOOM_HDMI_NATIVE_POINTER_TEST
#define PICODOOM_HDMI_NATIVE_POINTER_TEST 0
#endif

#ifndef PICODOOM_HDMI_SOLID_POINTER_TEST
#define PICODOOM_HDMI_SOLID_POINTER_TEST 0
#endif

#ifndef PICODOOM_HDMI_LINE_RING
#define PICODOOM_HDMI_LINE_RING 0
#endif

#ifndef PICODOOM_HDMI_LINE_RING_DIRECT
#define PICODOOM_HDMI_LINE_RING_DIRECT 0
#endif

#ifndef PICODOOM_HDMI_LINE_RING_SIZE
#define PICODOOM_HDMI_LINE_RING_SIZE 32
#endif

#ifndef PICODOOM_HDMI_LINE_RING_PREPARE
#define PICODOOM_HDMI_LINE_RING_PREPARE 1
#endif

#ifndef PICODOOM_HDMI_LINE_RING_VBLANK_ONLY
#define PICODOOM_HDMI_LINE_RING_VBLANK_ONLY 1
#endif

#ifndef PICODOOM_HDMI_CMDLIST
#define PICODOOM_HDMI_CMDLIST 0
#endif

#ifndef PICODOOM_HDMI_LITE
#define PICODOOM_HDMI_LITE 0
#endif

#if PICODOOM_HDMI_LITE
// pico_hdmi transport with all per-line work moved out of the ISR:
// pre-composed island line headers (ring at 0x20070000), native 16-bit
// pixel pointers into the RGB565 frame, audio pumped from the game mixer.
#if PICODOOM_HDMI_CMDLIST
#error PICODOOM_HDMI_LITE and PICODOOM_HDMI_CMDLIST are mutually exclusive
#endif
#if PICODOOM_HDMI_240P
#error PICODOOM_HDMI_LITE requires the 640x480 mode (PICODOOM_HDMI_240P=0)
#endif
#if PICODOOM_HDMI_DIAG_STAGE < 3
#error PICODOOM_HDMI_LITE requires PICODOOM_HDMI_DIAG_STAGE=3
#endif
#include "i_picosound.h"
#include "hdmi_lite_layout.h"
#endif

#if PICODOOM_HDMI_CMDLIST
// Command-list scanout: per-frame DMA command list, no per-line ISR, no
// pico_hdmi runtime. Scans the native 320x200 RGB565 frame directly.
#if PICODOOM_HDMI_240P
#error PICODOOM_HDMI_CMDLIST requires the 640x480 mode (PICODOOM_HDMI_240P=0)
#endif
#if PICODOOM_HDMI_DIAG_STAGE < 3
#error PICODOOM_HDMI_CMDLIST requires PICODOOM_HDMI_DIAG_STAGE=3
#endif
#if PICODOOM_HDMI_LINE_RING
#error PICODOOM_HDMI_CMDLIST replaces the line ring; set PICODOOM_HDMI_LINE_RING=0
#endif
#include "hstx_cmdlist.h"
#endif

#if PICODOOM_HDMI_CMDLIST || PICODOOM_HDMI_LITE
#define PICODOOM_HDMI_USE_RGB565_FRAME 1
#elif PICODOOM_HDMI_PREPARED_SCANLINES && PICODOOM_HDMI_DIAG_STAGE >= 3
#define PICODOOM_HDMI_USE_RGB565_FRAME 1
#else
#define PICODOOM_HDMI_USE_RGB565_FRAME 0
#endif

#define PICODOOM_HDMI_LINE_RING_ACTIVE \
    (PICODOOM_HDMI_LINE_RING && !PICODOOM_HDMI_240P && \
     (PICODOOM_HDMI_USE_RGB565_FRAME || PICODOOM_HDMI_LINE_RING_DIRECT))

#if PICODOOM_HDMI_USE_RGB565_FRAME
#define HDMI_SCANLINE_RENDER_ATTR
#else
#define HDMI_SCANLINE_RENDER_ATTR __scratch_x("doom_scanline")
#endif

#if PICODOOM_HDMI_240P
// 1280x240 output with 320x200 content: 4x horizontal, 1x vertical.
#define HDMI_H_ACTIVE 1280
#define HDMI_V_ACTIVE 240
#define LETTERBOX_TOP 20
#define LETTERBOX_BOTTOM 220
#define GAME_V_LINES 200
#else
// 640x480 output with 320x200 content: 2x horizontal, 2x vertical.
#define HDMI_H_ACTIVE 640
#define HDMI_V_ACTIVE 480
#define LETTERBOX_TOP 40
#define LETTERBOX_BOTTOM 440
#define GAME_V_LINES 400
#endif

// display has been set up?

static boolean initialized = false;

boolean screenvisible = true;

// The screen buffer; this is modified to draw things to the screen
// Gamma correction level to use

boolean screensaver_mode = false;

isb_int8_t usegamma = 0;

// Joystick/gamepad hysteresis
unsigned int joywait = 0;

pixel_t *I_VideoBuffer; // todo can't have this

uint8_t __aligned(4) frame_buffer[2][SCREENWIDTH*MAIN_VIEWHEIGHT];
#if defined(PICODOOM_HDMI_DIAG_STAGE) && PICODOOM_HDMI_DIAG_STAGE >= 3
#if PICODOOM_HDMI_LITE
// Parked in the top-of-SRAM region (hdmi_lite_layout.h) so the zone keeps
// enough headroom for the sound system's allocations.
uint8_t *const hdmi_status_buffer = (uint8_t *)HDMI_LITE_STATUS_BUF_ADDR;
_Static_assert(SCREENWIDTH * 32 <= HDMI_LITE_STATUS_BUF_BYTES,
               "hdmi_status_buffer overflows its region slot");
#else
uint8_t __aligned(4) hdmi_status_buffer[SCREENWIDTH * 32];
#endif
#endif
static uint16_t palette[256];
static uint16_t __scratch_x("shared_pal") shared_pal[NUM_SHARED_PALETTES][16];
static int8_t next_pal=-1;

semaphore_t render_frame_ready, display_frame_freed;
semaphore_t core1_launch;
static volatile uint32_t hdmi_diag_vsync_count;
static volatile uint32_t hdmi_diag_frameconsume_count;

// Monotonic boot marker stored in scratch RAM with an inverse canary.
// We set this from main/startup/render checkpoints to locate the first stalled stage.
static volatile uint32_t __scratch_x("diag_state") hdmi_diag_init_marker;
static volatile uint32_t __scratch_x("diag_state") hdmi_diag_init_marker_inv;

static inline void hdmi_diag_marker_reset(void) {
    hdmi_diag_init_marker = 0;
    hdmi_diag_init_marker_inv = ~0u;
}

void hdmi_diag_boot_marker_set(uint32_t value) {
    uint32_t cur = hdmi_diag_boot_marker_get();
    if (value <= cur) {
        return;
    }
    hdmi_diag_init_marker = value;
    hdmi_diag_init_marker_inv = ~value;
}

uint32_t hdmi_diag_boot_marker_get(void) {
    uint32_t value = hdmi_diag_init_marker;
    if (hdmi_diag_init_marker_inv != ~value) {
        return 0;
    }
    return value;
}

uint8_t *text_screen_data;

#if USE_INTERP
static interp_hw_save_t interp0_save, interp1_save;
static boolean interp_updated;
static boolean need_save;

static inline void interp_save_static(interp_hw_t *interp, interp_hw_save_t *saver) {
    saver->accum[0] = interp->accum[0];
    saver->accum[1] = interp->accum[1];
    saver->base[0] = interp->base[0];
    saver->base[1] = interp->base[1];
    saver->base[2] = interp->base[2];
    saver->ctrl[0] = interp->ctrl[0];
    saver->ctrl[1] = interp->ctrl[1];
}

static inline void interp_restore_static(interp_hw_t *interp, interp_hw_save_t *saver) {
    interp->accum[0] = saver->accum[0];
    interp->accum[1] = saver->accum[1];
    interp->base[0] = saver->base[0];
    interp->base[1] = saver->base[1];
    interp->base[2] = saver->base[2];
    interp->ctrl[0] = saver->ctrl[0];
    interp->ctrl[1] = saver->ctrl[1];
}
#endif

void I_ShutdownGraphics(void)
{
}

//
// I_StartFrame
//
void I_StartFrame (void)
{
    // er?
}

//
// Set the window title
//

void I_SetWindowTitle(const char *title)
{
//    window_title = title;
}

//
// I_SetPalette
//
void I_SetPaletteNum(int doompalette)
{
    next_pal = doompalette;
}

//
// I_FinishUpdate
//
void I_FinishUpdate (void)
{
}

uint8_t display_frame_index;
uint8_t display_overlay_index;
uint8_t display_video_type;

typedef void (*scanline_func)(uint32_t *dest, int scanline);

static void scanline_func_none(uint32_t *dest, int scanline);
static void scanline_func_double(uint32_t *dest, int scanline);
static void scanline_func_single(uint32_t *dest, int scanline);
static void scanline_func_wipe(uint32_t *dest, int scanline);

scanline_func scanline_funcs[] = {
        scanline_func_none,     // VIDEO_TYPE_NONE
        NULL,                   // VIDEO_TYPE_TEXT (stubbed)
        scanline_func_single,   // VIDEO_TYPE_SAVING
        scanline_func_double,   // VIDEO_TYPE_DOUBLE
        scanline_func_single,   // VIDEO_TYPE_SINGLE
        scanline_func_wipe,     // VIDEO_TYPE_WIPE
};

uint8_t *wipe_yoffsets; // position of start of y in each column
int16_t *wipe_yoffsets_raw;
uint32_t *wipe_linelookup; // offset of each line from start of screenbuffer (can be negative for FB 1 to FB 0)
uint8_t next_video_type;
#if defined(PICODOOM_HDMI_DIAG_STAGE) && PICODOOM_HDMI_DIAG_STAGE >= 3
// Pre-demotion video type (stage 3 demotes DOUBLE/WIPE to SINGLE for the
// lighter scanline path; the bottom-32-row source still depends on this).
static uint8_t display_video_type_raw;
#endif
uint8_t next_frame_index; // todo combine with video type?
uint8_t next_overlay_index;
#if !DEMO1_ONLY
uint8_t *next_video_scroll;
uint8_t *video_scroll;
#endif
volatile uint8_t wipe_min;

#pragma GCC push_options
#if PICO_ON_DEVICE
#pragma GCC optimize("O3")
#endif

static inline void palette_convert_scanline(uint32_t *dest, const uint8_t *src) {
#if USE_INTERP
    if (interp_updated != 1) {
                if (need_save) {
                    interp_save_static(interp0, &interp0_save);
                    interp_save_static(interp1, &interp1_save);
                }
                interp_config c = interp_default_config();
                interp_config_set_shift(&c, 0);
                interp_config_set_mask(&c, 0, 7);
                interp_set_config(interp0, 0, &c);
                interp_config_set_shift(&c, 16);
                interp_set_config(interp1, 0, &c);
                interp_config_set_shift(&c, 8);
                interp_config_set_cross_input(&c, true);
                interp_set_config(interp0, 1, &c);
                interp_config_set_shift(&c, 24);
                interp_set_config(interp1, 1, &c);
                uint32_t palette_div2 = ((uintptr_t)palette) >> 1;
                interp0->base[0] = palette_div2;
                interp0->base[1] = palette_div2;
                interp1->base[0] = palette_div2;
                interp1->base[1] = palette_div2;
                interp_updated = 1;
            }
            extern void palette8to16(uint32_t *dest, const uint8_t *src, uint words);
            palette8to16(dest, src, SCREENWIDTH);
//            dest[4] = (255-scanline) * 0x2000;
            dest += SCREENWIDTH / 2;
//            dest[-4] = (255-scanline) * 0x10001;
#else
    for (int i = 0; i < SCREENWIDTH; i += 2) {
        uint32_t val = palette[*src++];
        val |= (palette[*src++]) << 16;
        *dest++ = val;
    }
#endif
}

static void scanline_func_none(uint32_t *dest, int scanline) {
    memset(dest, 0, SCREENWIDTH * 2);
}

static void __scratch_x("doom_scanline") scanline_func_double(uint32_t *dest, int scanline) {
    if (scanline < MAIN_VIEWHEIGHT) {
        const uint8_t *src = frame_buffer[display_frame_index] + scanline * SCREENWIDTH;
        palette_convert_scanline(dest, src);
    } else {
        // we expect everything to be overdrawn by statusbar so we do nothing
    }
}

static void __scratch_x("doom_scanline") scanline_func_single(uint32_t *dest, int scanline) {
    uint8_t *src;
    if (scanline < MAIN_VIEWHEIGHT) {
        src = frame_buffer[display_frame_index] + scanline * SCREENWIDTH;
    } else {
#if defined(PICODOOM_HDMI_DIAG_STAGE) && PICODOOM_HDMI_DIAG_STAGE >= 3
        if (display_video_type_raw == VIDEO_TYPE_DOUBLE ||
            display_video_type_raw == VIDEO_TYPE_WIPE) {
            // Demoted gameplay frame: bottom rows are the pre-rendered
            // status bar.
            src = hdmi_status_buffer + (scanline - MAIN_VIEWHEIGHT) * SCREENWIDTH;
        } else {
            // True full-screen image (splash/title/help): the bottom 32 rows
            // live at the end of the other framebuffer.
            src = frame_buffer[display_frame_index^1] + (scanline - 32) * SCREENWIDTH;
        }
#else
        src = frame_buffer[display_frame_index^1] + (scanline - 32) * SCREENWIDTH;
#endif
    }
#if !DEMO1_ONLY
    if (video_scroll) {
        for(int i=SCREENWIDTH-1;i>0;i--) {
            src[i] = src[i-1];
        }
        src[0] = video_scroll[scanline];
    }
#endif
    palette_convert_scanline(dest, src);
}

static void scanline_func_wipe(uint32_t *dest, int scanline) {
    const uint8_t *src;
    if (scanline < MAIN_VIEWHEIGHT) {
        src = frame_buffer[display_frame_index];
    } else {
        src = frame_buffer[display_frame_index^1] - 32 * SCREENWIDTH;
    }
    assert(wipe_yoffsets && wipe_linelookup);
    uint16_t *d = (uint16_t *)dest;
    src += scanline * SCREENWIDTH;
    for (int i = 0; i < SCREENWIDTH; i++) {
        int rel = scanline - wipe_yoffsets[i];
        if (rel < 0) {
            d[i] = palette[src[i]];
        } else {
            const uint8_t *flip;
#if PICO_ON_DEVICE
            flip = (const uint8_t *)wipe_linelookup[rel];
#else
            flip = &frame_buffer[0][0] + wipe_linelookup[rel];
#endif
            // todo better protection here
            if (flip >= &frame_buffer[0][0] && flip < &frame_buffer[0][0] + 2 * SCREENWIDTH * MAIN_VIEWHEIGHT) {
                d[i] = palette[flip[i]];
            }
        }
    }
}

static inline uint draw_vpatch(uint16_t *dest, patch_t *patch, vpatchlist_t *vp, uint off) {
    int repeat = vp->entry.repeat;
    dest += vp->entry.x;
    int w = vpatch_width(patch);
    const uint8_t *data0 = vpatch_data(patch);
    const uint8_t *data = data0 + off;
    if (!vpatch_has_shared_palette(patch)) {
        const uint8_t *pal = vpatch_palette(patch);
        switch (vpatch_type(patch)) {
            case vp4_runs: {
                uint16_t *p = dest;
                uint16_t *pend = dest + w;
                uint8_t gap;
                while (0xff != (gap = *data++)) {
                    p += gap;
                    int len = *data++;
                    for (int i = 1; i < len; i += 2) {
                        uint v = *data++;
                        *p++ = palette[pal[v & 0xf]];
                        *p++ = palette[pal[v >> 4]];
                    }
                    if (len & 1) {
                        *p++ = palette[pal[(*data++) & 0xf]];
                    }
                    assert(p <= pend);
                    if (p == pend) break;
                }
                break;
            }
            case vp4_alpha: {
                uint16_t *p = dest;
                for (int i = 0; i < w / 2; i++) {
                    uint v = *data++;
                    if (v & 0xf) p[0] = palette[pal[v & 0xf]];
                    if (v >> 4) p[1] = palette[pal[v >> 4]];
                    p += 2;
                }
                if (w & 1) {
                    uint v = *data++;
                    if (v & 0xf) p[0] = palette[pal[v & 0xf]];
                }
                break;
            }
            case vp4_solid: {
                uint16_t *p = dest;
                for (int i = 0; i < w / 2; i++) {
                    uint v = *data++;
                    p[0] = palette[pal[v & 0xf]];
                    p[1] = palette[pal[v >> 4]];
                    p += 2;
                }
                if (w & 1) {
                    uint v = *data++;
                    p[0] = palette[pal[v & 0xf]];
                }
                break;
            }
            case vp6_runs: {
                uint16_t *p = dest;
                uint16_t *pend = dest + w;
                uint8_t gap;
                while (0xff != (gap = *data++)) {
                    p += gap;
                    int len = *data++;
                    for (int i = 3; i < len; i += 4) {
                        uint v = *data++;
                        v |= (*data++) << 8;
                        v |= (*data++) << 16;
                        *p++ = palette[pal[v & 0x3f]];
                        *p++ = palette[pal[(v >> 6) & 0x3f]];
                        *p++ = palette[pal[(v >> 12) & 0x3f]];
                        *p++ = palette[pal[(v >> 18) & 0x3f]];
                    }
                    len &= 3;
                    if (len--) {
                        uint v = *data++;
                        *p++ = palette[pal[v & 0x3f]];
                        if (len--) {
                            v >>= 6;
                            v |= (*data++) << 2;
                            *p++ = palette[pal[v & 0x3f]];
                            if (len--) {
                                v >>= 6;
                                v |= (*data++) << 4;
                                *p++ = palette[pal[v & 0x3f]];
                                assert(!len);
                            }
                        }
                    }
                    assert(p <= pend);
                    if (p == pend) break;
                }
                break;
            }
            case vp8_runs: {
                uint16_t *p = dest;
                uint16_t *pend = dest + w;
                uint8_t gap;
                while (0xff != (gap = *data++)) {
                    p += gap;
                    int len = *data++;
                    for (int i = 0; i < len; i++) {
                        *p++ = palette[pal[*data++]];
                    }
                    assert(p <= pend);
                    if (p == pend) break;
                }
                break;
            }
            case vp_border: {
                dest[0] = palette[*data++];
                uint16_t col = palette[*data++];
                for (int i = 1; i < w - 1; i++) dest[i] = col;
                dest[w-1] = palette[*data++];
                break;
            }
            default:
                assert(false);
                break;
        }
    } else {
        uint sp = vpatch_shared_palette(patch);
        uint16_t *pal16 = shared_pal[sp];
        assert(sp < NUM_SHARED_PALETTES);
        switch (vpatch_type(patch)) {
            case vp4_solid: {
#if PICO_ON_DEVICE
                if (patch == stbar) {
                    static const uint8_t *cached_data;
                    // short of scratch space on RP2350, so use main RAM
                    static uint32_t data_cache[41];
                    int i = 0;
                    uint32_t *d = (uint32_t *) dest;
#define DMA_CHANNEL 11
                    if (cached_data == data) {
                        const uint8_t *source = (const uint8_t *) data_cache;
                        // we need to correct for the misalignment of data, because the XIP copy ignores the low 2 bits...
                        // the raw bitmap data is always misaligned by 3 (the size of the header in the case of stbar)
                        source += 3;
                        for (; source < (const uint8_t *) dma_hw->ch[DMA_CHANNEL].al1_write_addr; source++) {
                            uint32_t val = pal16[source[0] & 0xf];
                            val |= (pal16[source[0] >> 4]) << 16;
                            *d++ = val;
                        }
                        source -= 3;
                        i = (source - (const uint8_t *) data_cache);
                    }
                    if (true) {
                        xip_ctrl_hw->stream_ctr = 0;
                        // workaround yucky bug
#if !PICO_RP2350
                        (void) *(io_rw_32 *) XIP_NOCACHE_NOALLOC_BASE;
                        xip_ctrl_hw->stream_fifo;
#endif
                        dma_channel_abort(DMA_CHANNEL);
                        dma_channel_config c = dma_channel_get_default_config(DMA_CHANNEL);
                        channel_config_set_read_increment(&c, false);
                        channel_config_set_write_increment(&c, true);
                        channel_config_set_dreq(&c, DREQ_XIP_STREAM);
#if !PICO_RP2350
                        dma_channel_set_read_addr(DMA_CHANNEL, (void *) XIP_AUX_BASE, false);
#else
                        dma_channel_set_read_addr(DMA_CHANNEL, &xip_ctrl_hw->stream_fifo, false);
#endif
                        dma_channel_set_config(DMA_CHANNEL, &c, false);
                        cached_data = data + SCREENWIDTH / 2;
                        xip_ctrl_hw->stream_addr = (uintptr_t) cached_data;
                        xip_ctrl_hw->stream_ctr = 41;
                        __compiler_memory_barrier();
                        dma_channel_transfer_to_buffer_now(DMA_CHANNEL, data_cache, 41);
                    }
                    for (; i < SCREENWIDTH / 2; i++) {
                        uint32_t val = pal16[data[i] & 0xf];
                        val |= (pal16[data[i] >> 4]) << 16;
                        *d++ = val;
                    }
                    data += SCREENWIDTH / 2;
                    break; // early break from switch
                }
#endif
                if (((uintptr_t)dest)&3) {
                    uint16_t *p = dest;
                    for (int i = 0; i < w / 2; i++) {
                        uint v = *data++;
                        p[0] = pal16[v & 0xf];
                        p[1] = pal16[v >> 4];
                        p += 2;
                    }
                } else {
                    uint32_t *wide = (uint32_t *) dest;
                    for (int i = 0; i < w / 2; i++) {
                        uint v = *data++;
                        wide[i] = pal16[v & 0xf] | (pal16[v >> 4] << 16);
                    }
                }
                if (w & 1) {
                    uint v = *data++;
                    dest[w-1] = pal16[v & 0xf];
                }
                break;
            }
            case vp4_alpha: {
                uint16_t *p = dest;
                for (int i = 0; i < w / 2; i++) {
                    uint v = *data++;
                    if (v & 0xf) p[0] = pal16[v & 0xf];
                    if (v >> 4) p[1] = pal16[v >> 4];
                    p += 2;
                }
                if (w & 1) {
                    uint v = *data++;
                    if (v & 0xf) p[0] = pal16[v & 0xf];
                }
                break;
            }
            default:
                assert(false);
        }
    }
    if (repeat) {
        // we need them to be solid... which they are, but if not you'll just get some visual funk
        if (vp->entry.patch_handle == VPATCH_M_THERMM) w--; // hackity hack
        for(int i=0;i<repeat*w;i++) {
            dest[w+i] = dest[i];
        }
    }
    return data - data0;
}

// this is not in flash as quite large and only once per frame
void __noinline new_frame_init_overlays_palette_and_wipe() {
    // re-initialize our overlay drawing
    if (display_video_type >= FIRST_VIDEO_TYPE_WITH_OVERLAYS) {
        memset(vpatchlists->vpatch_next, 0, sizeof(vpatchlists->vpatch_next));
        memset(vpatchlists->vpatch_starters, 0, sizeof(vpatchlists->vpatch_starters));
        memset(vpatchlists->vpatch_doff, 0, sizeof(vpatchlists->vpatch_doff));
        vpatchlist_t *overlays = vpatchlists->overlays[display_overlay_index];
        // do it in reverse so our linked lists are in ascending order
        for (int i = overlays->header.size - 1; i > 0; i--) {
            assert(overlays[i].entry.y < count_of(vpatchlists->vpatch_starters));
            vpatchlists->vpatch_next[i] = vpatchlists->vpatch_starters[overlays[i].entry.y];
            vpatchlists->vpatch_starters[overlays[i].entry.y] = i;
        }
        if (next_pal != -1) {
            static const uint8_t *playpal;
            static bool calculate_palettes;
            if (!playpal) {
                lumpindex_t l = W_GetNumForName("PLAYPAL");
                playpal = W_CacheLumpNum(l, PU_STATIC);
                calculate_palettes = W_LumpLength(l) == 768;
            }
            if (!calculate_palettes || !next_pal) {
                const uint8_t *doompalette = playpal + next_pal * 768;
                for (int i = 0; i < 256; i++) {
                    int r = *doompalette++;
                    int g = *doompalette++;
                    int b = *doompalette++;
                    if (usegamma) {
                        r = gammatable[usegamma-1][r];
                        g = gammatable[usegamma-1][g];
                        b = gammatable[usegamma-1][b];
                    }
                    palette[i] = PIXEL_FROM_RGB8(r, g, b);
                }
            } else {
                int mul, r0, g0, b0;
                if (next_pal < 9) {
                    mul = next_pal * 65536 / 9;
                    r0 = 255; g0 = b0 = 0;
                } else if (next_pal < 13) {
                    mul = (next_pal - 8) * 65536 / 8;
                    r0 = 215; g0 = 186; b0 = 69;
                } else {
                    mul = 65536 / 8;
                    r0 = b0 = 0; g0 = 256;
                }
                const uint8_t *doompalette = playpal;
                for (int i = 0; i < 256; i++) {
                    int r = *doompalette++;
                    int g = *doompalette++;
                    int b = *doompalette++;
                    r += ((r0 - r) * mul) >> 16;
                    g += ((g0 - g) * mul) >> 16;
                    b += ((b0 - b) * mul) >> 16;
                    palette[i] = PIXEL_FROM_RGB8(r, g, b);
                }
            }
            next_pal = -1;
            if (!stbar) {
                stbar = resolve_vpatch_handle(VPATCH_STBAR);
            }
            assert(vpatch_type(stbar) == vp4_solid); // no transparent, no runs, 4 bpp
            for (int i = 0; i < NUM_SHARED_PALETTES; i++) {
                patch_t *patch = resolve_vpatch_handle(vpatch_for_shared_palette[i]);
                assert(vpatch_colorcount(patch) <= 16);
                assert(vpatch_has_shared_palette(patch));
                for (int j = 0; j < 16; j++) {
                    shared_pal[i][j] = palette[vpatch_palette(patch)[j]];
                }
            }
        }
        if (display_video_type == VIDEO_TYPE_WIPE) {
            if (wipe_min <= 200) {
                bool regular = display_overlay_index; // just happens to toggle every frame
                int new_wipe_min = 200;
                for (int i = 0; i < SCREENWIDTH; i++) {
                    int v;
                    if (wipe_yoffsets_raw[i] < 0) {
                        if (regular) {
                            wipe_yoffsets_raw[i]++;
                        }
                        v = 0;
                    } else {
                        int dy = (wipe_yoffsets_raw[i] < 16) ? (1 + wipe_yoffsets_raw[i] + regular) / 2 : 4;
                        if (wipe_yoffsets_raw[i] + dy > 200) {
                            v = 200;
                        } else {
                            wipe_yoffsets_raw[i] += dy;
                            v = wipe_yoffsets_raw[i];
                        }
                    }
                    wipe_yoffsets[i] = v;
                    if (v < new_wipe_min) new_wipe_min = v;
                }
                assert(new_wipe_min >= wipe_min);
                wipe_min = new_wipe_min;
            }
        }
    }
}

#if PICODOOM_HDMI_USE_RGB565_FRAME
static void hdmi_rgb565_build_display_frame(void);
#endif

// this method moved out of scratchx because we didn't have quite enough space for core1 stack
void __no_inline_not_in_flash_func(new_frame_stuff)() {
    // this part of the per frame code is in RAM as it is needed during save
    bool frame_consumed = false;
    if (sem_available(&render_frame_ready)) {
        sem_acquire_blocking(&render_frame_ready);
        frame_consumed = true;
        hdmi_diag_frameconsume_count++;
        display_video_type = next_video_type;
#if defined(PICODOOM_HDMI_DIAG_STAGE) && PICODOOM_HDMI_DIAG_STAGE >= 3
        // Stage 3 stability mode: prefer the lightest scanline path. Keep
        // the pre-demotion type: scanline_func_single must source the bottom
        // 32 rows from the status buffer for demoted gameplay frames, but
        // from the other framebuffer for true full-screen images (splash).
        display_video_type_raw = display_video_type;
        if (display_video_type == VIDEO_TYPE_DOUBLE || display_video_type == VIDEO_TYPE_WIPE) {
            display_video_type = VIDEO_TYPE_SINGLE;
        }
#endif
        display_frame_index = next_frame_index;
        display_overlay_index = next_overlay_index;
#if !DEMO1_ONLY
        video_scroll = next_video_scroll; // todo does this waste too much space
#endif
    } else {
#if !DEMO1_ONLY
        video_scroll = NULL;
#endif
    }
    if (display_video_type != VIDEO_TYPE_SAVING) {
        // this stuff is large (so in flash) and not needed in save move
        new_frame_init_overlays_palette_and_wipe();
    }
#if PICODOOM_HDMI_USE_RGB565_FRAME
    if (frame_consumed) {
        hdmi_rgb565_build_display_frame();
    }
#endif
    if (frame_consumed) {
        sem_release(&display_frame_freed);
    }
}

// Temporary buffer for native 320px scanline before 2x expansion
static uint32_t __not_in_flash("scanline_temp") scanline_temp[SCREENWIDTH / 2]; // 160 words = 320 uint16_t
static uint32_t __not_in_flash("solid_line") solid_line[HDMI_H_ACTIVE / 2];
static bool hdmi_solid_scanout_initialized;
static bool hdmi_scanline_buffer_is_black;

#if PICODOOM_HDMI_FIFO_PROBE
// HSTX FIFO starvation probe. Sampled in DMA-ISR-context scanline callbacks:
// stat bit 9 (EMPTY) set during active video means the FIFO drained -- the
// underflow that drops sync; bits 7:0 (LEVEL) are the remaining margin.
// Because a sync drop kills the display, the result is also rendered ON SCREEN:
// the letterbox bars are green while clean and latch sticky-red the first time
// an underflow is seen during real game content. hdmi_fifo_probe_report() (Core
// 0, once/sec) additionally prints over UART if one is attached.
static uint32_t __not_in_flash("fifo_health") hdmi_fifo_health_line[HDMI_H_ACTIVE / 2];
static volatile bool hdmi_fifo_health_red;
static bool hdmi_fifo_health_inited;
static volatile uint32_t hdmi_fifo_probe_samples;
static volatile uint32_t hdmi_fifo_probe_empty_events;
static volatile uint32_t hdmi_fifo_probe_min_level = 0xffffffffu;

static void __not_in_flash_func(hdmi_fifo_health_fill)(uint16_t color) {
    uint32_t packed = color | ((uint32_t)color << 16);
    for (int i = 0; i < HDMI_H_ACTIVE / 2; i++) {
        hdmi_fifo_health_line[i] = packed;
    }
}

static void __not_in_flash_func(hdmi_fifo_probe_sample)(void) {
    if (!hdmi_fifo_health_inited) {
        hdmi_fifo_health_fill(0x07e0); // green: running, no underflow yet
        hdmi_fifo_health_inited = true;
    }
    uint32_t stat = hstx_fifo_hw->stat;
    uint32_t level = stat & 0xffu;
    if (level < hdmi_fifo_probe_min_level) {
        hdmi_fifo_probe_min_level = level;
    }
    hdmi_fifo_probe_samples++;
    if (stat & (1u << 9)) {            // EMPTY: FIFO drained on this active line
        hdmi_fifo_probe_empty_events++;
        // Latch red only once real game content is up, to ignore startup/resync
        // transients during the NONE/title phase.
        if (!hdmi_fifo_health_red && display_video_type != VIDEO_TYPE_NONE) {
            hdmi_fifo_health_red = true;
            hdmi_fifo_health_fill(0xf800); // red, sticky
        }
    }
}

void hdmi_fifo_probe_report(void) {
    static uint32_t frames;
    if (++frames < 60) {               // roughly once per second at 60 fps
        return;
    }
    frames = 0;
    printf("HSTX FIFO probe: empty=%u min_level=%u samples=%u red=%u\n",
           (unsigned)hdmi_fifo_probe_empty_events,
           (unsigned)(hdmi_fifo_probe_min_level == 0xffffffffu ? 0u : hdmi_fifo_probe_min_level),
           (unsigned)hdmi_fifo_probe_samples,
           (unsigned)hdmi_fifo_health_red);
    hdmi_fifo_probe_empty_events = 0;
    hdmi_fifo_probe_min_level = 0xffffffffu;
    hdmi_fifo_probe_samples = 0;
}
#else
void hdmi_fifo_probe_report(void) {}
#endif

#if PICODOOM_HDMI_USE_RGB565_FRAME
static uint32_t __aligned(4) hdmi_rgb565_frame[SCREENHEIGHT][SCREENWIDTH / 2];
static volatile bool hdmi_rgb565_frame_ready;
static bool hdmi_rgb565_frame_frozen;
#endif

#if PICODOOM_HDMI_LINE_RING_ACTIVE
#if PICODOOM_HDMI_LINE_RING_SIZE < 8
#error PICODOOM_HDMI_LINE_RING_SIZE must be at least 8
#endif
#define HDMI_RGB565_LINE_RING_LEAD (PICODOOM_HDMI_LINE_RING_SIZE - 4)

typedef struct {
    uint32_t pixels[HDMI_H_ACTIVE / 2];
    volatile uint16_t epoch;
    volatile uint16_t doom_line;
    volatile uint8_t ready;
} hdmi_rgb565_line_ring_entry_t;

static hdmi_rgb565_line_ring_entry_t __aligned(4) hdmi_rgb565_line_ring[PICODOOM_HDMI_LINE_RING_SIZE];
static uint32_t __aligned(4) hdmi_rgb565_line_ring_no_frame_line[HDMI_H_ACTIVE / 2];
static uint32_t __aligned(4) hdmi_rgb565_line_ring_building_line[HDMI_H_ACTIVE / 2];
static uint32_t __aligned(4) hdmi_rgb565_line_ring_missing_line[HDMI_H_ACTIVE / 2];
static volatile uint16_t hdmi_rgb565_line_ring_epoch = 1;
static volatile int16_t hdmi_rgb565_line_ring_scan_doom_line = -1;
static volatile bool hdmi_rgb565_line_ring_restart_pending;
static uint16_t hdmi_rgb565_line_ring_service_epoch;
static int16_t hdmi_rgb565_line_ring_prepare_next;

static inline void hdmi_rgb565_line_ring_bump_epoch(void) {
    uint16_t next = hdmi_rgb565_line_ring_epoch + 1u;
    if (!next) {
        next = 1;
    }
    hdmi_rgb565_line_ring_epoch = next;
    hdmi_rgb565_line_ring_scan_doom_line = -1;
}

static void hdmi_rgb565_line_ring_fill_diag_line(uint32_t *line, uint16_t color) {
    const uint32_t packed = color | ((uint32_t)color << 16);
    for (int i = 0; i < HDMI_H_ACTIVE / 2; i++) {
        line[i] = packed;
    }
}
#endif

uint32_t hdmi_diag_rgb565_frame_ready(void) {
#if PICODOOM_HDMI_USE_RGB565_FRAME
    return hdmi_rgb565_frame_ready ? 1u : 0u;
#else
    return 0;
#endif
}

static inline void fill_color_bars(uint32_t *line_buffer) {
    static const uint16_t bars[8] = {
        0xf800, // red
        0xfc00, // orange
        0xffe0, // yellow
        0x07e0, // green
        0x07ff, // cyan
        0x001f, // blue
        0xf81f, // magenta
        0xffff  // white
    };
    for (int bar = 0; bar < 8; bar++) {
        uint32_t px = bars[bar];
        uint32_t packed = px | (px << 16);
        int start = bar * (HDMI_H_ACTIVE / 16);
        int end = start + (HDMI_H_ACTIVE / 16);
        for (int i = start; i < end; i++) {
            line_buffer[i] = packed;
        }
    }
}

static inline uint16_t diag_counter_color(uint32_t count, uint16_t bright, uint16_t dim) {
    if (!count) {
        return 0xf800; // red: this stage has never happened
    }
    return (count & 0x10u) ? bright : dim;
}

static inline uint16_t diag_flag_color(bool set, uint16_t on) {
    return set ? on : 0xf800;
}

static inline uint16_t diag_video_type_color(void) {
    if (display_video_type != VIDEO_TYPE_NONE) {
        return 0x07e0; // green: actively displaying non-NONE video type
    }
    if (next_video_type != VIDEO_TYPE_NONE) {
        return 0xffe0; // yellow: producer selected a mode, consumer hasn't switched yet
    }
    return 0xf800; // red: still NONE on both sides
}

static inline void fill_diag_state_bars(uint32_t *line_buffer) {
    uint16_t colors[4] = {
            diag_counter_color(hdmi_diag_vsync_count, 0x07e0, 0x0220),           // VSYNC callback activity
            diag_counter_color(hdmi_diag_pd_publish_count, 0x001f, 0x0010),      // frame publish activity (blue)
            diag_counter_color(hdmi_diag_frameconsume_count, 0x07ff, 0x03ef),    // frame consume activity (cyan)
            diag_video_type_color(),                                              // NONE/next/display mode state
    };

    const int words_per_line = HDMI_H_ACTIVE / 2;
    const int words_per_bar = words_per_line / 4;
    for (int bar = 0; bar < 4; bar++) {
        uint32_t px = colors[bar];
        uint32_t packed = px | (px << 16);
        int start = bar * words_per_bar;
        int end = start + words_per_bar;
        if (bar == 3) {
            end = words_per_line;
        }
        for (int i = start; i < end; i++) {
            line_buffer[i] = packed;
        }
    }
}

static inline void fill_black_line_once(uint32_t *line_buffer) {
    if (!hdmi_scanline_buffer_is_black) {
        memset(line_buffer, 0, HDMI_H_ACTIVE * 2);
        hdmi_scanline_buffer_is_black = true;
    }
}

static void HDMI_SCANLINE_RENDER_ATTR hdmi_render_native_scanline(uint32_t *native_line, int doom_line) {
#if USE_INTERP
    need_save = interp_in_use;
    interp_updated = 0;
#endif

    scanline_func fn = scanline_funcs[display_video_type];
    if (fn) {
        fn(native_line, doom_line);
    } else {
        // TEXT mode stub: just show black.
        memset(native_line, 0, SCREENWIDTH * 2);
    }

    // Draw overlays on the native 320px line. Stage 3 uses the pre-rendered
    // status buffer, so keep this out of the critical stability path.
    if (display_video_type >= FIRST_VIDEO_TYPE_WITH_OVERLAYS && PICODOOM_HDMI_DIAG_STAGE < 3) {
        assert(doom_line < count_of(vpatchlists->vpatch_starters));
        int prev = 0;
        for (int vp = vpatchlists->vpatch_starters[doom_line]; vp;) {
            int next = vpatchlists->vpatch_next[vp];
            while (vpatchlists->vpatch_next[prev] && vpatchlists->vpatch_next[prev] < vp) {
                prev = vpatchlists->vpatch_next[prev];
            }
            assert(prev != vp);
            assert(vpatchlists->vpatch_next[prev] != vp);
            vpatchlists->vpatch_next[vp] = vpatchlists->vpatch_next[prev];
            vpatchlists->vpatch_next[prev] = vp;
            prev = vp;
            vp = next;
        }
        vpatchlist_t *overlays = vpatchlists->overlays[display_overlay_index];
        prev = 0;
        for (int vp = vpatchlists->vpatch_next[prev]; vp; vp = vpatchlists->vpatch_next[prev]) {
            patch_t *patch = resolve_vpatch_handle(overlays[vp].entry.patch_handle);
            int yoff = doom_line - overlays[vp].entry.y;
            if (yoff < vpatch_height(patch)) {
                vpatchlists->vpatch_doff[vp] = draw_vpatch((uint16_t*)native_line, patch, &overlays[vp],
                                                           vpatchlists->vpatch_doff[vp]);
                prev = vp;
            } else {
                vpatchlists->vpatch_next[prev] = vpatchlists->vpatch_next[vp];
            }
        }
    }

#if USE_INTERP
    if (interp_updated && need_save) {
        interp_restore_static(interp0, &interp0_save);
        interp_restore_static(interp1, &interp1_save);
    }
#endif
}

static inline void HDMI_SCANLINE_RENDER_ATTR hdmi_expand_native_scanline(uint32_t *out, const uint32_t *native_line) {
    const uint16_t *src16 = (const uint16_t *)native_line;
#if PICODOOM_HDMI_240P
    for (int i = 0; i < SCREENWIDTH; i++) {
        uint32_t px = src16[i];
        uint32_t packed = px | (px << 16);
        out[i * 2] = packed;
        out[i * 2 + 1] = packed;
    }
#else
    for (int i = 0; i < SCREENWIDTH; i++) {
        uint32_t px = src16[i];
        out[i] = px | (px << 16);
    }
#endif
}

#if PICODOOM_HDMI_LINE_RING_ACTIVE
static void hdmi_rgb565_line_ring_prepare_one(int doom_line) {
    hdmi_rgb565_line_ring_entry_t *entry =
        &hdmi_rgb565_line_ring[(uint)doom_line % PICODOOM_HDMI_LINE_RING_SIZE];

    entry->ready = 0;
    __compiler_memory_barrier();
#if PICODOOM_HDMI_USE_RGB565_FRAME
    hdmi_expand_native_scanline(entry->pixels, hdmi_rgb565_frame[doom_line]);
#else
    hdmi_render_native_scanline(scanline_temp, doom_line);
    hdmi_expand_native_scanline(entry->pixels, scanline_temp);
#endif
    entry->doom_line = (uint16_t)doom_line;
    entry->epoch = hdmi_rgb565_line_ring_service_epoch;
    __compiler_memory_barrier();
    entry->ready = 1;
}

static void hdmi_rgb565_line_ring_service(void) {
    if (display_video_type == VIDEO_TYPE_NONE) {
        return;
    }
#if PICODOOM_HDMI_USE_RGB565_FRAME
    if (!hdmi_rgb565_frame_ready) {
        return;
    }
#endif

    const uint16_t epoch = hdmi_rgb565_line_ring_epoch;
    const int16_t scan_doom_line = hdmi_rgb565_line_ring_scan_doom_line;

    if (hdmi_rgb565_line_ring_restart_pending || hdmi_rgb565_line_ring_service_epoch != epoch) {
        hdmi_rgb565_line_ring_restart_pending = false;
        hdmi_rgb565_line_ring_service_epoch = epoch;
        hdmi_rgb565_line_ring_prepare_next = 0;
    } else if (scan_doom_line >= 0 && hdmi_rgb565_line_ring_prepare_next < scan_doom_line) {
        hdmi_rgb565_line_ring_prepare_next = scan_doom_line;
    }

    int16_t max_prepare = scan_doom_line < 0
        ? HDMI_RGB565_LINE_RING_LEAD
        : (int16_t)(scan_doom_line + HDMI_RGB565_LINE_RING_LEAD);
    if (max_prepare >= SCREENHEIGHT) {
        max_prepare = SCREENHEIGHT - 1;
    }

    for (int budget = 0; budget < 2 && hdmi_rgb565_line_ring_prepare_next <= max_prepare; budget++) {
        hdmi_rgb565_line_ring_prepare_one(hdmi_rgb565_line_ring_prepare_next);
        hdmi_rgb565_line_ring_prepare_next++;
    }
}

static const uint32_t *__scratch_x("doom_scanline") hdmi_rgb565_line_ring_pointer_callback(uint32_t v_scanline, uint32_t active_line) {
    (void)v_scanline;
#if PICODOOM_HDMI_FIFO_PROBE
    hdmi_fifo_probe_sample();
#endif

    if (active_line < LETTERBOX_TOP || active_line >= LETTERBOX_BOTTOM) {
#if PICODOOM_HDMI_FIFO_PROBE
        return hdmi_fifo_health_line; // green=clean, sticky-red once underflow seen
#else
        return solid_line;
#endif
    }

    const int content_line = (int)active_line - LETTERBOX_TOP;
    const int doom_line = content_line / 2;
    if (doom_line < 0 || doom_line >= SCREENHEIGHT) {
        return hdmi_rgb565_line_ring_missing_line;
    }

    if (display_video_type == VIDEO_TYPE_NONE) {
        return hdmi_rgb565_line_ring_no_frame_line;
    }

#if PICODOOM_HDMI_USE_RGB565_FRAME
    if (!hdmi_rgb565_frame_ready) {
        return hdmi_rgb565_line_ring_building_line;
    }
#endif

    hdmi_rgb565_line_ring_scan_doom_line = (int16_t)doom_line;

    const uint16_t epoch = hdmi_rgb565_line_ring_epoch;
    hdmi_rgb565_line_ring_entry_t *entry =
        &hdmi_rgb565_line_ring[(uint)doom_line % PICODOOM_HDMI_LINE_RING_SIZE];
    if (entry->ready && entry->epoch == epoch && entry->doom_line == (uint16_t)doom_line) {
        return entry->pixels;
    }

    return hdmi_rgb565_line_ring_missing_line;
}
#endif

#if PICODOOM_HDMI_USE_RGB565_FRAME
#if PICODOOM_HDMI_LITE && !PICODOOM_HDMI_DVI_MODE
static void hdmi_audio_pump(void);
#endif

static void hdmi_rgb565_build_display_frame(void) {
#if PICODOOM_HDMI_FREEZE_RGB565_FRAME
    if (hdmi_rgb565_frame_frozen) {
        return;
    }
#endif
#if PICODOOM_HDMI_LITE || PICODOOM_HDMI_CMDLIST
    // Direct-scan modes rebuild the frame in place while it is being scanned
    // out: a slow rebuild tears briefly. Clearing the ready latch here would
    // instead black out every row scanned during the ~3 ms rebuild.
#else
    hdmi_rgb565_frame_ready = false;
    __compiler_memory_barrier();
#endif

    if (display_video_type == VIDEO_TYPE_NONE) {
        return;
    }

    for (int doom_line = 0; doom_line < SCREENHEIGHT; doom_line++) {
        hdmi_render_native_scanline(hdmi_rgb565_frame[doom_line], doom_line);
#if PICODOOM_HDMI_LITE
        // The compose ring leads the beam by ~88 lines but this whole-frame
        // rebuild takes ~3 ms (~95 scanout lines): without mid-rebuild
        // service the ring goes stale and the ISR drops one audio packet per
        // stale line, every frame.
        if ((doom_line & 15) == 15) {
            video_output_compose_service();
#if !PICODOOM_HDMI_DVI_MODE
            hdmi_audio_pump();
#endif
        }
#endif
    }

#if PICODOOM_HDMI_LINE_RING_ACTIVE
    hdmi_rgb565_line_ring_bump_epoch();
    hdmi_rgb565_line_ring_restart_pending = true;
#endif
    __compiler_memory_barrier();
    hdmi_rgb565_frame_ready = true;
#if PICODOOM_HDMI_FREEZE_RGB565_FRAME
    hdmi_rgb565_frame_frozen = true;
#endif
}

#if !PICODOOM_HDMI_CMDLIST
static void __scratch_x("doom_scanline") hdmi_rgb565_scanline_callback(uint32_t v_scanline, uint32_t active_line, uint32_t *line_buffer) {
    (void)v_scanline;
#if PICODOOM_HDMI_FIFO_PROBE
    hdmi_fifo_probe_sample();
#endif

#if PICODOOM_FORCE_SOLID_SCANOUT && PICODOOM_SOLID_COLOR && PICODOOM_SOLID_COLOR != 0
    memcpy(line_buffer, solid_line, HDMI_H_ACTIVE * 2);
    return;
#endif

#if PICODOOM_SOLID_COLOR && PICODOOM_SOLID_COLOR != 0
    if (display_video_type == VIDEO_TYPE_NONE) {
        memcpy(line_buffer, solid_line, HDMI_H_ACTIVE * 2);
        return;
    }
#endif

    if (active_line < LETTERBOX_TOP || active_line >= LETTERBOX_BOTTOM ||
        display_video_type == VIDEO_TYPE_NONE || !hdmi_rgb565_frame_ready) {
        fill_black_line_once(line_buffer);
        return;
    }

    int content_line = active_line - LETTERBOX_TOP;
#if PICODOOM_HDMI_240P
    int doom_line = content_line;
#else
    if (content_line & 1) {
        return;
    }
    int doom_line = content_line / 2;
#endif

    hdmi_expand_native_scanline(line_buffer, hdmi_rgb565_frame[doom_line]);
    hdmi_scanline_buffer_is_black = false;
}

#if PICODOOM_HDMI_NATIVE_POINTER_TEST && !PICODOOM_HDMI_240P
static const uint32_t *__scratch_x("doom_scanline") hdmi_rgb565_native_pointer_callback(uint32_t v_scanline, uint32_t active_line) {
    (void)v_scanline;

    if (active_line < LETTERBOX_TOP || active_line >= LETTERBOX_BOTTOM ||
        display_video_type == VIDEO_TYPE_NONE || !hdmi_rgb565_frame_ready) {
        return solid_line;
    }

    int content_line = active_line - LETTERBOX_TOP;
    int doom_line = content_line / 2;
    if (doom_line < 0 || doom_line >= SCREENHEIGHT) {
        return solid_line;
    }

    // Diagnostic only: HSTX still reads 640 pixels, so this intentionally
    // scans two adjacent native 320px lines instead of doing 2x expansion.
    return hdmi_rgb565_frame[doom_line];
}
#endif
#endif // !PICODOOM_HDMI_CMDLIST
#endif

#if !PICODOOM_HDMI_CMDLIST
static const uint32_t *__scratch_x("doom_scanline") hdmi_solid_pointer_callback(uint32_t v_scanline, uint32_t active_line) {
    (void)v_scanline;
    (void)active_line;
    return solid_line;
}

#if PICODOOM_HDMI_LITE
// Native pixel mode: return the 320px RGB565 row; hardware doubles it.
static uint32_t __aligned(4) hdmi_lite_black_row[SCREENWIDTH / 2];
// On-screen text diagnostics (no UART needed): a 3-line counter dump in the
// top letterbox, rendered into a canvas in the free top-of-SRAM region.
// Counters keep their last values when something halts, so the frozen frame
// IS the post-mortem.
extern volatile uint32_t hdmi_diag_i_error_count;
extern volatile uint32_t hdmi_diag_checkpoint;

#define HDMI_LITE_TEXT_ROWS 24
#define hdmi_lite_text_canvas ((uint16_t (*)[SCREENWIDTH])HDMI_LITE_TEXT_CANVAS_ADDR)
_Static_assert(HDMI_LITE_TEXT_ROWS * SCREENWIDTH * 2 <= HDMI_LITE_TEXT_CANVAS_BYTES,
               "diag text canvas overflows its region slot");

// HSTX FIFO health, sampled per active line in ISR context: EMPTY during
// active video = underflow (the thing that drops sync); LEVEL = margin.
static volatile uint32_t hdmi_lite_fifo_empty_events;
static volatile uint32_t hdmi_lite_fifo_min_level = 0xff;

// 5x7 font, column bytes, bit0 = top row. Subset needed by the diag lines.
typedef struct { char c; uint8_t col[5]; } diag_glyph_t;
static const diag_glyph_t hdmi_lite_font[] = {
    {'0', {0x3E,0x51,0x49,0x45,0x3E}}, {'1', {0x00,0x42,0x7F,0x40,0x00}},
    {'2', {0x42,0x61,0x51,0x49,0x46}}, {'3', {0x21,0x41,0x45,0x4B,0x31}},
    {'4', {0x18,0x14,0x12,0x7F,0x10}}, {'5', {0x27,0x45,0x45,0x45,0x39}},
    {'6', {0x3C,0x4A,0x49,0x49,0x30}}, {'7', {0x01,0x71,0x09,0x05,0x03}},
    {'8', {0x36,0x49,0x49,0x49,0x36}}, {'9', {0x06,0x49,0x49,0x29,0x1E}},
    {'B', {0x7F,0x49,0x49,0x49,0x36}}, {'C', {0x3E,0x41,0x41,0x41,0x22}},
    {'E', {0x7F,0x49,0x49,0x49,0x41}}, {'F', {0x7F,0x09,0x09,0x09,0x01}},
    {'H', {0x7F,0x08,0x08,0x08,0x7F}}, {'K', {0x7F,0x08,0x14,0x22,0x41}},
    {'L', {0x7F,0x40,0x40,0x40,0x40}}, {'M', {0x7F,0x02,0x0C,0x02,0x7F}},
    {'N', {0x7F,0x04,0x08,0x10,0x7F}}, {'P', {0x7F,0x09,0x09,0x09,0x06}},
    {'R', {0x7F,0x09,0x19,0x29,0x46}}, {'S', {0x46,0x49,0x49,0x49,0x31}},
    {'T', {0x01,0x01,0x7F,0x01,0x01}},
    {'X', {0x63,0x14,0x08,0x14,0x63}}, {'Y', {0x07,0x08,0x70,0x08,0x07}},
};

static void hdmi_lite_draw_text(int x, int y, const char *s) {
    for (; *s && x + 6 <= SCREENWIDTH; s++, x += 6) {
        if (*s == ' ') continue;
        const uint8_t *col = NULL;
        for (uint i = 0; i < count_of(hdmi_lite_font); i++) {
            if (hdmi_lite_font[i].c == *s) { col = hdmi_lite_font[i].col; break; }
        }
        if (!col) continue;
        for (int cx = 0; cx < 5; cx++) {
            for (int cy = 0; cy < 7; cy++) {
                if (col[cx] & (1 << cy)) {
                    hdmi_lite_text_canvas[y + cy][x + cx] = 0xffff;
                }
            }
        }
    }
}

static void hdmi_lite_update_diag_rows(void) {
    char line[56];
    memset(hdmi_lite_text_canvas, 0, HDMI_LITE_TEXT_ROWS * SCREENWIDTH * 2);
    snprintf(line, sizeof line, "LP %lu TC %d ER %lu CP %lu",
             (unsigned long)hdmi_diag_doomloop_count, gametic,
             (unsigned long)hdmi_diag_i_error_count,
             (unsigned long)hdmi_diag_checkpoint);
    hdmi_lite_draw_text(0, 0, line);
#if defined(USB_SUPPORT) && USB_SUPPORT
    // USB keyboard bring-up: device mounts / HID interfaces / kbd reports.
    // PL stays on as the audio heartbeat.
    {
        extern volatile uint32_t hdmi_diag_usb_mount_count;
        extern volatile uint32_t hdmi_diag_hid_mount_count;
        extern volatile uint32_t hdmi_diag_kbd_report_count;
        snprintf(line, sizeof line, "KB M %lu H %lu R %lu SL %lu",
                 (unsigned long)hdmi_diag_usb_mount_count,
                 (unsigned long)hdmi_diag_hid_mount_count,
                 (unsigned long)hdmi_diag_kbd_report_count,
                 (unsigned long)hstx_di_queue_silence_count);
    }
#else
    snprintf(line, sizeof line, "MX %lu PL %lu FE %lu FL %lu",
             (unsigned long)I_PicoSoundMixedCount(),
             (unsigned long)I_PicoSoundPulledCount(),
             (unsigned long)hdmi_lite_fifo_empty_events,
             (unsigned long)hdmi_lite_fifo_min_level);
#endif
    hdmi_lite_draw_text(0, 8, line);
    snprintf(line, sizeof line, "PB %lu CN %lu RY %d FR %d ST %lu",
             (unsigned long)hdmi_diag_pd_publish_count,
             (unsigned long)hdmi_diag_frameconsume_count,
             sem_available(&render_frame_ready),
             sem_available(&display_frame_freed),
             (unsigned long)video_output_precomposed_stale_count);
    hdmi_lite_draw_text(0, 16, line);
}

static const uint32_t *__scratch_x("doom_scanline") hdmi_lite_pointer_callback(uint32_t v_scanline, uint32_t active_line) {
    (void)v_scanline;
    {
        uint32_t stat = hstx_fifo_hw->stat;
        uint32_t level = stat & 0xffu;
        if (level < hdmi_lite_fifo_min_level) {
            hdmi_lite_fifo_min_level = level;
        }
        if (stat & (1u << 9)) {
            hdmi_lite_fifo_empty_events++;
        }
    }
    // Diag text overlays the top of the picture area (TV overscan clips the
    // letterbox region).
    if (active_line >= 56 && active_line < 56 + HDMI_LITE_TEXT_ROWS) {
        return (const uint32_t *)hdmi_lite_text_canvas[active_line - 56];
    }
    if (active_line < LETTERBOX_TOP || active_line >= LETTERBOX_BOTTOM ||
        display_video_type == VIDEO_TYPE_NONE || !hdmi_rgb565_frame_ready) {
        return hdmi_lite_black_row;
    }
    return hdmi_rgb565_frame[(active_line - LETTERBOX_TOP) / 2];
}
#endif
#endif

#if !PICODOOM_HDMI_USE_RGB565_FRAME
static void __scratch_x("doom_scanline") hdmi_scanline_callback(uint32_t v_scanline, uint32_t active_line, uint32_t *line_buffer) {
    (void)v_scanline;
#if PICODOOM_HDMI_FIFO_PROBE
    hdmi_fifo_probe_sample();
#endif

    if (PICODOOM_HDMI_DIAG_STAGE == 1) {
        fill_color_bars(line_buffer);
        return;
    }
    if (PICODOOM_HDMI_DIAG_STAGE == 2) {
        fill_diag_state_bars(line_buffer);
        return;
    }

#if PICODOOM_FORCE_SOLID_SCANOUT && PICODOOM_SOLID_COLOR && PICODOOM_SOLID_COLOR != 0
    if (!hdmi_solid_scanout_initialized) {
        memcpy(line_buffer, solid_line, HDMI_H_ACTIVE * 2);
        hdmi_solid_scanout_initialized = true;
    }
    return;
#endif

#if PICODOOM_SOLID_COLOR && PICODOOM_SOLID_COLOR != 0
    // Fallback: show solid color until Doom publishes a real frame
    if (display_video_type == 0 /* VIDEO_TYPE_NONE */) {
        memcpy(line_buffer, solid_line, HDMI_H_ACTIVE * 2);
        return;
    }
#endif

    // Letterbox bars: top 40 lines and bottom 40 lines are black
    if (active_line < LETTERBOX_TOP || active_line >= LETTERBOX_BOTTOM) {
        fill_black_line_once(line_buffer);
        return;
    }
    if (display_video_type == VIDEO_TYPE_NONE) {
        fill_black_line_once(line_buffer);
        return;
    }

    // Game content: map active output lines to 200 Doom lines.
    int content_line = active_line - LETTERBOX_TOP;
#if PICODOOM_HDMI_240P
    int doom_line = content_line;
#else
    // For 480p vertical 2x scaling, odd lines can reuse the previous expanded line.
    if (content_line & 1) {
        return;
    }
    int doom_line = content_line / 2;
#endif

    hdmi_render_native_scanline(scanline_temp, doom_line);
    hdmi_expand_native_scanline(line_buffer, scanline_temp);
    hdmi_scanline_buffer_is_black = false;
}
#endif

static volatile bool hdmi_vsync_pending;
static bool led_state;
static uint32_t hdmi_blink_counter;

void hdmi_diag_service_video_handoff(void) {
    if (!get_core_num()) {
        return;
    }
    if (hdmi_vsync_pending) {
        hdmi_vsync_pending = false;
        new_frame_stuff();
    }
}

#if !PICODOOM_HDMI_CMDLIST
#if !PICODOOM_HDMI_DVI_MODE && (PICODOOM_HDMI_AUDIO_TEST_TONE || PICODOOM_HDMI_LITE)
// Pump audio packets into pico_hdmi's data-island queue (the transport the
// bouncing_box demos prove works with audio). Runs in Core 1's background
// task between scanline IRQs. Source: the game mixer ring, or a 1 kHz
// square wave when PICODOOM_HDMI_AUDIO_TEST_TONE is set.
static void hdmi_audio_pump(void) {
    static int iec_counter;
#if PICODOOM_HDMI_AUDIO_TEST_TONE
    static uint32_t tone_phase;
#endif
    // 200 packets ~= one frame of audio cushion -- the level the demo that
    // plays clean music keeps. 64 left underruns possible during Core 1's
    // busy windows, and each underrun inserts a silence packet (stream
    // stretch + grit).
    while (hstx_di_queue_get_level() < 200) {
        audio_sample_t samples[4];
#if PICODOOM_HDMI_AUDIO_TEST_TONE == 3
        // 750 Hz pure sine (48000/64 samples). A sine exposes every dropped,
        // repeated or corrupted sample as audible grit -- the square tone
        // masks exactly those artifacts. Quarter-wave table, amp ~37% FS.
        static const int16_t qsin[17] = {0, 1176, 2341, 3483, 4592, 5657,
                                         6667, 7613, 8485, 9276, 9978, 10583,
                                         11087, 11483, 11769, 11942, 12000};
        for (int i = 0; i < 4; i++) {
            uint32_t p = tone_phase++ & 63u;
            int16_t v = (p <= 16)   ? qsin[p]
                        : (p <= 32) ? qsin[32 - p]
                        : (p <= 48) ? (int16_t)-qsin[p - 32]
                                    : (int16_t)-qsin[64 - p];
            samples[i].left = v;
            samples[i].right = v;
        }
#elif PICODOOM_HDMI_AUDIO_TEST_TONE
        for (int i = 0; i < 4; i++) {
            int16_t v = ((tone_phase++ / 24u) & 1u) ? 3000 : -3000;
            samples[i].left = v;
            samples[i].right = v;
        }
#else
        I_PicoSoundPullStereo((int16_t *)samples, 4);
#endif
        hstx_packet_t packet;
        hstx_data_island_t island;
        iec_counter = hstx_packet_set_audio_samples_cs(&packet, samples, 4, iec_counter);
        hstx_encode_data_island(&island, &packet, false, true);
        hstx_di_queue_push(&island);
    }
}
#endif

static void hdmi_vsync_callback(void) {
    // VSYNC callback runs in DMA IRQ context; defer heavy frame work.
    hdmi_diag_vsync_count++;
#if PICODOOM_HDMI_LINE_RING_ACTIVE
    hdmi_rgb565_line_ring_scan_doom_line = -1;
    hdmi_rgb565_line_ring_restart_pending = true;
#endif
    hdmi_vsync_pending = true;
    if (++hdmi_blink_counter >= 30) {
        hdmi_blink_counter = 0;
        led_state = !led_state;
        gpio_put(PICO_DEFAULT_LED_PIN, led_state);
    }
}

static void core1_background_task(void) {
#if PICODOOM_HDMI_LITE
    // Keep pre-composed active-line headers and the island queue topped up
    // BEFORE the (long) deferred frame work so the ring covers the rebuild.
    video_output_compose_service();
#endif
#if !PICODOOM_HDMI_DVI_MODE && (PICODOOM_HDMI_AUDIO_TEST_TONE || PICODOOM_HDMI_LITE)
    hdmi_audio_pump();
#endif
#if PICODOOM_HDMI_LITE
    if (hdmi_vsync_pending) {
        // On-screen telemetry only. NO printf anywhere on Core 1: a single
        // UART line blocks ~7 ms on the stdio path and starves everything
        // else this task does (measured as a 1 Hz audio glitch before the
        // ISR took over the island schedule).
        hdmi_lite_update_diag_rows();
    }
#endif
    hdmi_diag_service_video_handoff();
#if PICODOOM_HDMI_LINE_RING_ACTIVE && PICODOOM_HDMI_LINE_RING_PREPARE
#if PICODOOM_HDMI_LINE_RING_VBLANK_ONLY
    if (video_output_in_vertical_blanking()) {
        hdmi_rgb565_line_ring_service();
    }
#else
    hdmi_rgb565_line_ring_service();
#endif
#endif
}
#endif

#pragma GCC pop_options

#if PICODOOM_HDMI_CMDLIST
static void core1() {
    // Bus priority is (re)set inside hstx_cmdlist_scanout_start; with no
    // per-line ISR the scanout DMA is the only real-time agent on this core.
    hstx_cmdlist_scanout_start((const uint32_t *)hdmi_rgb565_frame, SCREENWIDTH / 2, SCREENHEIGHT);
    sem_release(&core1_launch);
    uint32_t last_frame = ~0u;
    while (true) {
        uint32_t frame = hstx_cmdlist_frame_number();
        if (frame == last_frame) {
            // Keep the active-line audio island ring filled ahead of the beam.
            hstx_cmdlist_audio_poll();
            continue;
        }
        last_frame = frame;
        hdmi_diag_vsync_count++;
        if (++hdmi_blink_counter >= 30) {
            hdmi_blink_counter = 0;
            led_state = !led_state;
            gpio_put(PICO_DEFAULT_LED_PIN, led_state);
        }
        // Prefill the whole audio ring while the beam is still in blanking --
        // it covers the video rebuild below, during which we cannot poll.
        hstx_cmdlist_audio_frame_begin();
        // Frame handoff plus the full 320x200 RGB565 rebuild (in
        // hdmi_rgb565_build_display_frame), once per scanout frame. Scanout
        // never waits on this: a slow rebuild can tear, not drop sync.
        new_frame_stuff();
    }
}
#else
static void core1() {
    // Prefer HSTX DMA over CPU bus traffic; video_output_core1_run() sets the
    // final DMA read/write priority again after DMA setup.
    busctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_R_BITS;
    while (!(busctrl_hw->priority_ack)) tight_loop_contents();

#if PICODOOM_HDMI_LITE
    video_output_set_scanline_pointer_callback(hdmi_lite_pointer_callback);
    video_output_set_native_pixel_mode(true);
    // Pre-composed line ring in the free top-of-SRAM region (layout and
    // overlap asserts in hdmi_lite_layout.h -- note vpatchlists owns the
    // top 3 KB). 88 entries lead the beam by ~80 lines; the per-frame
    // RGB565 rebuild additionally services the ring mid-loop so it can
    // never fully stale out.
    video_output_set_compose_ring((video_output_precomposed_line_t *)HDMI_LITE_COMPOSE_RING_ADDR,
                                  HDMI_LITE_COMPOSE_RING_ENTRIES);
    // Audio pacing: the library default (48 kHz over all 525 lines, set by
    // configure_audio_packets) is correct -- packets must be spread across
    // blanking too. Pacing over only the 480 active lines leaves a 1.4 ms
    // delivery hole every frame = 60 Hz sidebands on everything.
#elif PICODOOM_HDMI_SOLID_POINTER_TEST
    video_output_set_scanline_pointer_callback(hdmi_solid_pointer_callback);
#elif PICODOOM_HDMI_LINE_RING_ACTIVE
    video_output_set_scanline_pointer_callback(hdmi_rgb565_line_ring_pointer_callback);
#elif PICODOOM_HDMI_USE_RGB565_FRAME
#if PICODOOM_HDMI_NATIVE_POINTER_TEST && !PICODOOM_HDMI_240P
    video_output_set_scanline_pointer_callback(hdmi_rgb565_native_pointer_callback);
#else
    video_output_set_scanline_callback(hdmi_rgb565_scanline_callback);
#endif
#else
    video_output_set_scanline_callback(hdmi_scanline_callback);
#endif
    video_output_set_vsync_callback(hdmi_vsync_callback);
    video_output_set_background_task(core1_background_task);
    sem_release(&core1_launch);
    video_output_core1_run(); // does not return
}
#endif

#if PICO_RP2350
#include "hardware/structs/accessctrl.h"
#endif
void I_InitGraphics(void)
{
    if (initialized) {
        return;
    }
    hdmi_diag_marker_reset();
    hdmi_solid_scanout_initialized = false;
#if PICODOOM_HDMI_USE_RGB565_FRAME
    hdmi_rgb565_frame_ready = false;
    hdmi_rgb565_frame_frozen = false;
#endif
#if PICODOOM_HDMI_LINE_RING_ACTIVE
    hdmi_rgb565_line_ring_epoch = 1;
    hdmi_rgb565_line_ring_scan_doom_line = -1;
    hdmi_rgb565_line_ring_service_epoch = 0;
    hdmi_rgb565_line_ring_prepare_next = 0;
    for (int i = 0; i < PICODOOM_HDMI_LINE_RING_SIZE; i++) {
        hdmi_rgb565_line_ring[i].ready = 0;
    }
    hdmi_rgb565_line_ring_fill_diag_line(hdmi_rgb565_line_ring_no_frame_line, 0x001f);
    hdmi_rgb565_line_ring_fill_diag_line(hdmi_rgb565_line_ring_building_line, 0xffe0);
    hdmi_rgb565_line_ring_fill_diag_line(hdmi_rgb565_line_ring_missing_line, 0xf81f);
#endif
    stbar = NULL;
    sem_init(&render_frame_ready, 0, 2);
    sem_init(&display_frame_freed, 1, 2);
    sem_init(&core1_launch, 0, 1);
    pd_init();
#if !PICODOOM_HDMI_CMDLIST
    hstx_di_queue_init();
    // We output a 640x480 HDMI timing and scale 320x200 content in the scanline callback.
    video_output_init(HDMI_H_ACTIVE, HDMI_V_ACTIVE);
    video_output_set_dvi_mode(PICODOOM_HDMI_DVI_MODE != 0);
#endif
    #if PICODOOM_SOLID_COLOR && (PICODOOM_SOLID_COLOR != 0)
    {
        const uint32_t solid_pixel = PICODOOM_SOLID_COLOR & 0xffffu;
        const uint32_t packed = solid_pixel | (solid_pixel << 16);
        for (int i = 0; i < HDMI_H_ACTIVE / 2; i++) {
            solid_line[i] = packed;
        }
    }
    #endif
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    printf("HDMI diag stage=%d dvi_mode=%d cmdlist=%d\n", PICODOOM_HDMI_DIAG_STAGE,
           PICODOOM_HDMI_DVI_MODE, PICODOOM_HDMI_CMDLIST);
    // Use a larger stack in main SRAM for Core 1.  The default SCRATCH_X
    // stack (0x4f8) is too small: pd_core1_loop()'s deep render call chain
    // plus the DMA ISR frame overflow it, corrupting HDMI output.
    static uint32_t core1_stack[2048]; // 8 KB
    multicore_launch_core1_with_stack(core1, core1_stack, sizeof(core1_stack));
    // wait for core1 launch as it may do malloc and we have no mutex around that
    sem_acquire_blocking(&core1_launch);
#if USE_ZONE_FOR_MALLOC
    disallow_core1_malloc = true;
#endif
#if PICO_RP2350
    hw_set_bits(&accessctrl_hw->xip_ctrl, ACCESSCTRL_PASSWORD_BITS | 0xff);
#endif
    initialized = true;
}

// Bind all variables controlling video options into the configuration
// file system.
void I_BindVideoVariables(void)
{
}

//
// I_StartTic
//
void I_StartTic (void)
{
    if (!initialized)
    {
        return;
    }

    I_GetEvent();
}


//
// I_UpdateNoBlit
//
void I_UpdateNoBlit (void)
{
    // what is this?
}

int I_GetPaletteIndex(int r, int g, int b)
{
    return 0;
}

#if !NO_USE_ENDDOOM
void I_Endoom(byte *endoom_data) {
    // ENDOOM text mode stubbed for HDMI — show black screen
    // Can be re-implemented later using pico_hdmi scanline callback
}
#endif

void I_GraphicsCheckCommandLine(void)
{
}

// Check if we have been invoked as a screensaver by xscreensaver.

void I_CheckIsScreensaver(void)
{
}

void I_DisplayFPSDots(boolean dots_on)
{
}

#endif
