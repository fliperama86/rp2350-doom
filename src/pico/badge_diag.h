#pragma once
#include <stdint.h>

// Crash record kept in watchdog scratch 0..3, which survive the watchdog
// reboot back into the klippy-cade launcher. The launcher shows it.
//   scratch0 = BADGE_DIAG_MAGIC | kind << 12 | stage
//   scratch1 = pc, scratch2 = lr, scratch3 = cfsr (fault) or fmt (panic)
#define BADGE_DIAG_MAGIC 0x4b4c0000u
#define BADGE_DIAG_HANG 1u
#define BADGE_DIAG_FAULT 2u
#define BADGE_DIAG_PANIC 3u

#define BADGE_STAGE_MAIN 100u
#define BADGE_STAGE_LCD_START 101u
#define BADGE_STAGE_LCD_DONE 102u
#define BADGE_STAGE_CORE1 103u
#define BADGE_STAGE_FIRST_FRAME 200u

void badge_diag_start(void);
void badge_diag_stage(uint32_t stage);
void badge_diag_feed(void);
void badge_diag_clear(void);
