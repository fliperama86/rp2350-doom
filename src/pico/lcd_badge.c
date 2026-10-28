// ST77916 QSPI panel on the SaintCon badge. Doom renders a native 320x200 RGB565
// frame; this centres it on the 360x360 round glass.
#include "lcd_badge.h"

#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#include "pico/stdlib.h"
#include "qspi_lcd.pio.h"
#include "st77916_init.h"

#define LCD_SCLK 10
#define LCD_D0 11
#define LCD_CS 15
#define LCD_RST 9
#define LCD_BL 7
#define LCD_TE 8
#define LCD_W 360
#define LCD_H 360

#define QSPI_OP_WRITE_CMD 0x02
#define QSPI_OP_WRITE_PIXELS 0x32

static PIO pio;
static uint sm;
static int dma_chan;
static uint16_t black_line[LCD_W];

static inline void fifo_put_byte(uint8_t v) {
    while (pio_sm_is_tx_fifo_full(pio, sm)) tight_loop_contents();
    *(volatile uint8_t *)&pio->txf[sm] = v;
}

static void send_single_line(uint8_t b) {
    for (int bit = 7; bit > 0; bit -= 2)
        fifo_put_byte((uint8_t)(((b >> bit) & 1) << 4 | ((b >> (bit - 1)) & 1)));
}

static void wait_bus_idle(void) {
    uint32_t stall = 1u << (PIO_FDEBUG_TXSTALL_LSB + sm);
    pio->fdebug = stall;
    while (!pio_sm_is_tx_fifo_empty(pio, sm) || !(pio->fdebug & stall)) tight_loop_contents();
}

static inline void cs(bool active) {
    gpio_put(LCD_CS, !active);
    if (!active) busy_wait_at_least_cycles(16);
}

static void write_command(uint8_t cmd, const uint8_t *data, size_t len) {
    cs(true);
    send_single_line(QSPI_OP_WRITE_CMD);
    send_single_line(0x00);
    send_single_line(cmd);
    send_single_line(0x00);
    for (size_t i = 0; i < len; i++) send_single_line(data[i]);
    wait_bus_idle();
    cs(false);
}

static void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    const uint8_t caset[] = {x0 >> 8, x0 & 0xff, x1 >> 8, x1 & 0xff};
    const uint8_t raset[] = {y0 >> 8, y0 & 0xff, y1 >> 8, y1 & 0xff};
    write_command(0x2a, caset, sizeof caset);
    write_command(0x2b, raset, sizeof raset);
}

static void dma_bytes(const void *src, uint32_t nbytes, bool increment) {
    dma_channel_config dc = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_8);
    channel_config_set_read_increment(&dc, increment);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_dreq(&dc, pio_get_dreq(pio, sm, true));
    dma_channel_configure(dma_chan, &dc, &pio->txf[sm], src, nbytes, true);
    dma_channel_wait_for_finish_blocking(dma_chan);
    wait_bus_idle();
}

static void fill_black(void) {
    set_window(0, 0, LCD_W - 1, LCD_H - 1);
    cs(true);
    send_single_line(QSPI_OP_WRITE_PIXELS);
    send_single_line(0x00);
    send_single_line(0x2c);
    send_single_line(0x00);
    for (int y = 0; y < LCD_H; y++) dma_bytes(black_line, sizeof black_line, true);
    cs(false);
}

void lcd_badge_init(void) {
    gpio_init(LCD_CS);
    gpio_put(LCD_CS, 1);
    gpio_set_dir(LCD_CS, GPIO_OUT);
    gpio_set_slew_rate(LCD_CS, GPIO_SLEW_RATE_FAST);
    gpio_init(LCD_RST);
    gpio_set_dir(LCD_RST, GPIO_OUT);
    gpio_init(LCD_TE);
    gpio_set_dir(LCD_TE, GPIO_IN);
    gpio_pull_up(LCD_TE);

    // This build links pico_float_none: every float op is a panic stub, so the
    // clock dividers below are fixed point.
    uint64_t sys_hz = clock_get_hz(clk_sys);

    gpio_set_function(LCD_BL, GPIO_FUNC_PWM);
    pwm_config pc = pwm_get_default_config();
    pwm_config_set_wrap(&pc, 999);
    uint32_t pwm_div16 = (uint32_t)(sys_hz * 16u / (1000u * 20000u));
    pwm_config_set_clkdiv_int_frac4(&pc, pwm_div16 >> 4, pwm_div16 & 0xfu);
    pwm_init(pwm_gpio_to_slice_num(LCD_BL), &pc, true);
    pwm_set_gpio_level(LCD_BL, 800);

    // SCLK = clk_sys / (2 * div). div = clk_sys/150MHz keeps SCLK at the 75 MHz this
    // panel was run at; round up so it never runs faster.
    uint32_t pio_div256 = (uint32_t)((sys_hz * 256u + 150000000u - 1u) / 150000000u);
    if (pio_div256 < 256u) pio_div256 = 256u;

    uint offset;
    hard_assert(pio_claim_free_sm_and_add_program_for_gpio_range(&qspi_lcd_program, &pio, &sm, &offset, LCD_SCLK, 5, true));
    for (uint i = 0; i < 4; i++) pio_gpio_init(pio, LCD_D0 + i);
    pio_gpio_init(pio, LCD_SCLK);
    pio_sm_set_consecutive_pindirs(pio, sm, LCD_D0, 4, true);
    pio_sm_set_consecutive_pindirs(pio, sm, LCD_SCLK, 1, true);
    pio_sm_config c = qspi_lcd_program_get_default_config(offset);
    sm_config_set_out_pins(&c, LCD_D0, 4);
    sm_config_set_sideset_pins(&c, LCD_SCLK);
    sm_config_set_out_shift(&c, false, true, 8);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv_int_frac8(&c, pio_div256 >> 8, pio_div256 & 0xffu);
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);
    for (int i = 0; i < 5; i++) gpio_set_slew_rate(LCD_SCLK + i, GPIO_SLEW_RATE_FAST);

    dma_chan = dma_claim_unused_channel(true);

    gpio_put(LCD_RST, 0);
    sleep_ms(100);
    gpio_put(LCD_RST, 1);
    sleep_ms(100);
    for (size_t i = 0; i < sizeof st77916_init_cmds / sizeof st77916_init_cmds[0]; i++) {
        const struct st77916_cmd *c = &st77916_init_cmds[i];
        write_command(c->cmd, c->data, c->len);
        if (c->delay_ms) sleep_ms(c->delay_ms);
    }
    fill_black();
}

void lcd_badge_begin(int w, int h) {
    int x0 = (LCD_W - w) / 2;
    int y0 = (LCD_H - h) / 2;
    set_window((uint16_t)x0, (uint16_t)y0, (uint16_t)(x0 + w - 1), (uint16_t)(y0 + h - 1));
    cs(true);
    send_single_line(QSPI_OP_WRITE_PIXELS);
    send_single_line(0x00);
    send_single_line(0x2c);
    send_single_line(0x00);
}

void lcd_badge_push_line(uint16_t *rgb565, int w) {
    // The renderer stores little-endian RGB565. The panel wants the high byte first.
    for (int i = 0; i < w; i++) rgb565[i] = (uint16_t)((rgb565[i] >> 8) | (rgb565[i] << 8));
    dma_bytes(rgb565, (uint32_t)w * 2u, true);
}

void lcd_badge_end(void) {
    cs(false);
}
