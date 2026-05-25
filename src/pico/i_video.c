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

#if PICODOOM_HDMI_PREPARED_SCANLINES && PICODOOM_HDMI_DIAG_STAGE >= 3
#define PICODOOM_HDMI_USE_RGB565_FRAME 1
#else
#define PICODOOM_HDMI_USE_RGB565_FRAME 0
#endif

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
uint8_t __aligned(4) hdmi_status_buffer[SCREENWIDTH * 32];
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
        src = hdmi_status_buffer + (scanline - MAIN_VIEWHEIGHT) * SCREENWIDTH;
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
        // Stage 3 stability mode: prefer the lightest scanline path.
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

#if PICODOOM_HDMI_USE_RGB565_FRAME
static uint32_t __aligned(4) hdmi_rgb565_frame[SCREENHEIGHT][SCREENWIDTH / 2];
static volatile bool hdmi_rgb565_frame_ready;
#endif

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

#if PICODOOM_HDMI_USE_RGB565_FRAME
static void hdmi_rgb565_build_display_frame(void) {
    hdmi_rgb565_frame_ready = false;
    __compiler_memory_barrier();

    if (display_video_type == VIDEO_TYPE_NONE) {
        return;
    }

    for (int doom_line = 0; doom_line < SCREENHEIGHT; doom_line++) {
        hdmi_render_native_scanline(hdmi_rgb565_frame[doom_line], doom_line);
    }

    __compiler_memory_barrier();
    hdmi_rgb565_frame_ready = true;
}

static void __scratch_x("doom_scanline") hdmi_rgb565_scanline_callback(uint32_t v_scanline, uint32_t active_line, uint32_t *line_buffer) {
    (void)v_scanline;

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
#endif

#if !PICODOOM_HDMI_USE_RGB565_FRAME
static void __scratch_x("doom_scanline") hdmi_scanline_callback(uint32_t v_scanline, uint32_t active_line, uint32_t *line_buffer) {
    (void)v_scanline;

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

static void hdmi_vsync_callback(void) {
    // VSYNC callback runs in DMA IRQ context; defer heavy frame work.
    hdmi_diag_vsync_count++;
    hdmi_vsync_pending = true;
    if (++hdmi_blink_counter >= 30) {
        hdmi_blink_counter = 0;
        led_state = !led_state;
        gpio_put(PICO_DEFAULT_LED_PIN, led_state);
    }
}

static void core1_background_task(void) {
    hdmi_diag_service_video_handoff();
}

#pragma GCC pop_options

static void core1() {
    // Prefer HSTX DMA over CPU bus traffic; video_output_core1_run() sets the
    // final DMA read/write priority again after DMA setup.
    busctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_R_BITS;
    while (!(busctrl_hw->priority_ack)) tight_loop_contents();

#if PICODOOM_HDMI_USE_RGB565_FRAME
    video_output_set_scanline_callback(hdmi_rgb565_scanline_callback);
#else
    video_output_set_scanline_callback(hdmi_scanline_callback);
#endif
    video_output_set_vsync_callback(hdmi_vsync_callback);
    video_output_set_background_task(core1_background_task);
    sem_release(&core1_launch);
    video_output_core1_run(); // does not return
}

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
#endif
    stbar = NULL;
    sem_init(&render_frame_ready, 0, 2);
    sem_init(&display_frame_freed, 1, 2);
    sem_init(&core1_launch, 0, 1);
    pd_init();
    hstx_di_queue_init();
    // We output a 640x480 HDMI timing and scale 320x200 content in the scanline callback.
    video_output_init(HDMI_H_ACTIVE, HDMI_V_ACTIVE);
    video_output_set_dvi_mode(PICODOOM_HDMI_DVI_MODE != 0);
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
    printf("HDMI diag stage=%d dvi_mode=%d\n", PICODOOM_HDMI_DIAG_STAGE, PICODOOM_HDMI_DVI_MODE);
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
