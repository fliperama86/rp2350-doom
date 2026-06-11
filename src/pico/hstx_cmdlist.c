//
// HSTX command-list scanout backend (PICODOOM_HDMI_CMDLIST=1).
//
// Replaces pico_hdmi's per-scanline-IRQ scanout with the picodvi approach
// used by CircuitPython and fruitjam-doom (MIT, Copyright (c) 2023 Scott
// Shawcroft for Adafruit Industries): the entire 640x480 frame -- sync,
// blanking and pixel lines -- is one precomputed DMA command list. A command
// channel writes the pixel channel's four al3 registers per slot
// (ring-wrapped, retriggered by chain), the pixel channel feeds the HSTX
// FIFO paced by DREQ_HSTX, and the only interrupt is one per frame to rewind
// the command list. There is no per-line CPU deadline to miss, which is
// exactly the pico_hdmi failure mode under live rendering (see
// INVESTIGATION_PROGRESS.md).
//
// Scanout reads the native 320x200 RGB565 frame directly: the pixel channel
// does 16-bit transfers (the bus replicates them across the 32-bit FIFO
// write, yielding px|(px<<16)) and the HSTX expander emits each word as two
// pixels (ENC_N_SHIFTS=2/ENC_SHIFT=16) for the 2x horizontal scale. The 2x
// vertical scale is each frame row appearing in two consecutive command
// slots. DVI only -- no data islands, so no HDMI audio.
//

#include "pico.h"

#if PICO_ON_DEVICE && PICODOOM_HDMI_CMDLIST

#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/busctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"

#include "hstx_cmdlist.h"

// 640x480@60 (VIC 1), negative sync, pixel clock = clk_hstx / 5 = 25.2 MHz
// with the usual 252 MHz / MODE_HSTX_CLK_DIV=2 clocking.
#define H_FRONT_PORCH   16
#define H_SYNC_WIDTH    96
#define H_BACK_PORCH    48
#define H_ACTIVE        640
#define V_FRONT_PORCH   10
#define V_SYNC_WIDTH    2
#define V_BACK_PORCH    33
#define V_ACTIVE        480
#define V_TOTAL         (V_FRONT_PORCH + V_SYNC_WIDTH + V_BACK_PORCH + V_ACTIVE)

#define CONTENT_WIDTH   320 // native pixels per line; HSTX doubles to 640
#define LETTERBOX_TOP   40

#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu

#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

#define HSTX_CMD_RAW_REPEAT (0x1u << 12)
#define HSTX_CMD_TMDS       (0x2u << 12)
#define HSTX_CMD_NOP        (0xfu << 12)

#define VBLANK_LINE_LEN 6
#define VACTIVE_LINE_LEN 9

// Everything DMA reads per scanline lives in the top 64 KB of SRAM
// (0x20070000-0x2007FFFF, banks 4-7): the linker never places anything there
// (__end__ stays below it, the zone allocator is capped at 0x20070000 by the
// shortptr window, and the stacks are in scratch X/Y), so it is the quietest
// region available for the scanout's hot data.
#define CMDLIST_RAM_BASE 0x20070000u

typedef struct {
    uint32_t vblank_vsync_off[VBLANK_LINE_LEN];
    uint32_t vblank_vsync_on[VBLANK_LINE_LEN];
    uint32_t vactive[VACTIVE_LINE_LEN];
    uint32_t black_line[CONTENT_WIDTH / 2];
    // Per scanline: one 4-word slot for the line's command sequence, plus a
    // second slot with the pixel transfer on active lines; one null-trigger
    // terminator slot raises the per-frame IRQ.
    uint32_t commands[(V_TOTAL - V_ACTIVE) * 4 + V_ACTIVE * 8 + 4];
} scanout_ram_t;

_Static_assert(sizeof(scanout_ram_t) <= 0x10000, "scanout data must fit the free top 64KB of SRAM");

#define scanout_ram ((scanout_ram_t *)CMDLIST_RAM_BASE)

static int dma_pixel_channel = -1;
static int dma_command_channel = -1;
static volatile uint32_t cmdlist_frame_counter;

uint32_t hstx_cmdlist_frame_number(void) {
    return cmdlist_frame_counter;
}

static void __not_in_flash_func(cmdlist_frame_irq)(void) {
    dma_irqn_acknowledge_channel(2, dma_pixel_channel);
    cmdlist_frame_counter++;
    // Rewind the command list; this retriggers the whole next frame.
    dma_hw->ch[dma_command_channel].al3_read_addr_trig = (uintptr_t)scanout_ram->commands;
}

void hstx_cmdlist_scanout_start(const uint32_t *frame_base, uint32_t pitch_words,
                                uint32_t frame_lines) {
    extern char __end__;
    hard_assert((uintptr_t)&__end__ <= CMDLIST_RAM_BASE);

    // clk_hstx = clk_sys / MODE_HSTX_CLK_DIV (252/2 = 126 MHz); the CSR
    // CLKDIV=5 below then yields the 25.2 MHz pixel clock.
    clock_configure_int_divider(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS,
                                clock_get_hz(clk_sys), MODE_HSTX_CLK_DIV);

    dma_pixel_channel = dma_claim_unused_channel(true);
    dma_command_channel = dma_claim_unused_channel(true);

    const uint32_t vblank_vsync_off[VBLANK_LINE_LEN] = {
        HSTX_CMD_RAW_REPEAT | H_FRONT_PORCH,
        SYNC_V1_H1,
        HSTX_CMD_RAW_REPEAT | H_SYNC_WIDTH,
        SYNC_V1_H0,
        HSTX_CMD_RAW_REPEAT | (H_BACK_PORCH + H_ACTIVE),
        SYNC_V1_H1,
    };
    const uint32_t vblank_vsync_on[VBLANK_LINE_LEN] = {
        HSTX_CMD_RAW_REPEAT | H_FRONT_PORCH,
        SYNC_V0_H1,
        HSTX_CMD_RAW_REPEAT | H_SYNC_WIDTH,
        SYNC_V0_H0,
        HSTX_CMD_RAW_REPEAT | (H_BACK_PORCH + H_ACTIVE),
        SYNC_V0_H1,
    };
    const uint32_t vactive[VACTIVE_LINE_LEN] = {
        HSTX_CMD_RAW_REPEAT | H_FRONT_PORCH,
        SYNC_V1_H1,
        HSTX_CMD_NOP,
        HSTX_CMD_RAW_REPEAT | H_SYNC_WIDTH,
        SYNC_V1_H0,
        HSTX_CMD_NOP,
        HSTX_CMD_RAW_REPEAT | H_BACK_PORCH,
        SYNC_V1_H1,
        HSTX_CMD_TMDS | H_ACTIVE,
    };
    memcpy(scanout_ram->vblank_vsync_off, vblank_vsync_off, sizeof(vblank_vsync_off));
    memcpy(scanout_ram->vblank_vsync_on, vblank_vsync_on, sizeof(vblank_vsync_on));
    memcpy(scanout_ram->vactive, vactive, sizeof(vactive));
    memset(scanout_ram->black_line, 0, sizeof(scanout_ram->black_line));

    const uint32_t fifo_addr = (uint32_t)&hstx_fifo_hw->fifo;
    const uint32_t ctrl_base =
        ((uint32_t)dma_command_channel << DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB) |
        ((uint32_t)DREQ_HSTX << DMA_CH0_CTRL_TRIG_TREQ_SEL_LSB) |
        DMA_CH0_CTRL_TRIG_IRQ_QUIET_BITS |
        DMA_CH0_CTRL_TRIG_INCR_READ_BITS |
        DMA_CH0_CTRL_TRIG_EN_BITS;
    const uint32_t ctrl_cmd32 = ctrl_base | ((uint32_t)DMA_SIZE_32 << DMA_CH0_CTRL_TRIG_DATA_SIZE_LSB);
    // 16-bit FIFO writes: the bus replicates the half-word across the 32-bit
    // register, so each native pixel arrives as px|(px<<16). No BSWAP -- the
    // frame is native little-endian RGB565 (same layout pico_hdmi scans).
    const uint32_t ctrl_px16 = ctrl_base | ((uint32_t)DMA_SIZE_16 << DMA_CH0_CTRL_TRIG_DATA_SIZE_LSB);

    uint32_t *cmd = scanout_ram->commands;
    const uint32_t active_start = V_SYNC_WIDTH + V_BACK_PORCH;
    const uint32_t active_end = active_start + V_ACTIVE;
    for (uint32_t v = 0; v < V_TOTAL; v++) {
        const uint32_t *line_seq;
        uint32_t line_seq_len;
        if (v < V_SYNC_WIDTH) {
            line_seq = scanout_ram->vblank_vsync_on;
            line_seq_len = VBLANK_LINE_LEN;
        } else if (v < active_start || v >= active_end) {
            line_seq = scanout_ram->vblank_vsync_off;
            line_seq_len = VBLANK_LINE_LEN;
        } else {
            line_seq = scanout_ram->vactive;
            line_seq_len = VACTIVE_LINE_LEN;
        }
        *cmd++ = ctrl_cmd32;
        *cmd++ = fifo_addr;
        *cmd++ = line_seq_len;
        *cmd++ = (uintptr_t)line_seq;
        if (line_seq_len == VACTIVE_LINE_LEN) {
            uint32_t active_line = v - active_start;
            const uint32_t *row;
            if (active_line < LETTERBOX_TOP || active_line >= LETTERBOX_TOP + 2 * frame_lines) {
                row = scanout_ram->black_line;
            } else {
                row = frame_base + ((active_line - LETTERBOX_TOP) / 2) * pitch_words;
            }
            *cmd++ = ctrl_px16;
            *cmd++ = fifo_addr;
            *cmd++ = CONTENT_WIDTH;
            *cmd++ = (uintptr_t)row;
        }
    }
    // Terminator: enabled + quiet with a zero-count null trigger raises the
    // per-frame IRQ; chain-to-self disables chaining.
    *cmd++ = DMA_CH0_CTRL_TRIG_IRQ_QUIET_BITS | DMA_CH0_CTRL_TRIG_EN_BITS |
             ((uint32_t)dma_pixel_channel << DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB);
    *cmd++ = 0;
    *cmd++ = 0;
    *cmd++ = 0;
    hard_assert(cmd == scanout_ram->commands + count_of(scanout_ram->commands));

    // HSTX config: identical to pico_hdmi's 480p RGB565 setup (expander pops
    // two pixels per FIFO word, TMDS lane rotations for little-endian 565).
    hstx_ctrl_hw->expand_tmds =
        4 << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB | 8 << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB |
        5 << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB | 3 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |
        4 << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB | 29 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;
    hstx_ctrl_hw->expand_shift =
        2 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB | 16 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB | 0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        5u << HSTX_CTRL_CSR_CLKDIV_LSB |
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
        2u << HSTX_CTRL_CSR_SHIFT_LSB |
        HSTX_CTRL_CSR_EN_BITS;

    // Pin mapping identical to pico_hdmi: CK- on GPIO12/CK+ on 13, lanes
    // D0..D2 on 14/15, 16/17, 18/19 (N on the even pin).
    hstx_ctrl_hw->bit[0] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    hstx_ctrl_hw->bit[1] = HSTX_CTRL_BIT0_CLK_BITS;
    for (uint lane = 0; lane < 3; ++lane) {
        int bit = 2 + lane * 2;
        uint32_t lane_data_sel_bits =
            (lane * 10) << HSTX_CTRL_BIT0_SEL_P_LSB |
            (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
        hstx_ctrl_hw->bit[bit] = lane_data_sel_bits | HSTX_CTRL_BIT0_INV_BITS;
        hstx_ctrl_hw->bit[bit + 1] = lane_data_sel_bits;
    }
    for (int pin = 12; pin <= 19; ++pin) {
        gpio_set_function(pin, 0); // HSTX
        gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);
        gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);
    }

    // Command channel: copies one 4-word slot into the pixel channel's al3
    // register block (ctrl, write_addr, trans_count, read_addr_trig); the
    // write ring keeps re-targeting those four registers, and the pixel
    // channel's chain re-triggers it after every slot.
    dma_channel_config c = dma_channel_get_default_config(dma_command_channel);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true, 4); // wrap write address every 16 bytes
    dma_channel_configure(dma_command_channel, &c,
                          &dma_hw->ch[dma_pixel_channel].al3_ctrl,
                          scanout_ram->commands, 4, false);

    dma_irqn_acknowledge_channel(2, dma_pixel_channel);
    dma_irqn_set_channel_enabled(2, dma_pixel_channel, true);
    irq_set_exclusive_handler(DMA_IRQ_2, cmdlist_frame_irq);
    irq_set_priority(DMA_IRQ_2, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_enabled(DMA_IRQ_2, true);

    // DMA wins per-cycle bus arbitration; with no per-line ISR it is the only
    // real-time agent in this scanout mode.
    busctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_R_BITS | BUSCTRL_BUS_PRIORITY_DMA_W_BITS;
    while (!busctrl_hw->priority_ack) tight_loop_contents();

    __compiler_memory_barrier();
    cmdlist_frame_irq(); // kick off the first frame
}

#endif // PICO_ON_DEVICE && PICODOOM_HDMI_CMDLIST
