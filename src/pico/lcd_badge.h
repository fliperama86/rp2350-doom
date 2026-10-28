#pragma once
#include <stdint.h>

void lcd_badge_init(void);
// One scanline at a time, centred. The full 320x200 frame does not fit
// alongside Doom's zone in the short-pointer window.
void lcd_badge_begin(int w, int h);
void lcd_badge_push_line(uint16_t *rgb565, int w);
void lcd_badge_end(void);
