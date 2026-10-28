#include "badge_diag.h"

#include "hardware/structs/scb.h"
#include "hardware/structs/systick.h"
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#include "pico.h"

#define BADGE_DIAG_WATCHDOG_MS 8000u

static uint32_t current_stage;
static bool started;

static void store(uint32_t kind, uint32_t pc, uint32_t lr, uint32_t extra) {
    watchdog_hw->scratch[0] = BADGE_DIAG_MAGIC | (kind << 12) | (current_stage & 0xfffu);
    watchdog_hw->scratch[1] = pc;
    watchdog_hw->scratch[2] = lr;
    watchdog_hw->scratch[3] = extra;
}

void badge_diag_start(void) {
    current_stage = BADGE_STAGE_MAIN;
    store(BADGE_DIAG_HANG, 0, 0, 0);
    // The launcher armed the watchdog before the jump; watchdog_update() would
    // reload from this image's zeroed copy of the load value, so re-arm here.
    watchdog_enable(BADGE_DIAG_WATCHDOG_MS, true);
    started = true;
    // Sample core 0's PC so a hang still says where it was spinning.
    systick_hw->rvr = 0x00ffffffu;
    systick_hw->cvr = 0;
    systick_hw->csr = 0x7u;
}

void badge_diag_stage(uint32_t stage) {
    current_stage = stage;
    uint32_t s0 = watchdog_hw->scratch[0];
    uint32_t kind = (s0 >> 12) & 0xfu;
    if ((s0 & 0xffff0000u) != BADGE_DIAG_MAGIC) kind = BADGE_DIAG_HANG;
    watchdog_hw->scratch[0] = BADGE_DIAG_MAGIC | (kind << 12) | (stage & 0xfffu);
    badge_diag_feed();
}

void badge_diag_feed(void) {
    if (started) watchdog_update();
}

void badge_diag_clear(void) {
    watchdog_hw->scratch[0] = 0;
}

void __used badge_diag_sample(uint32_t *frame) {
    if (((watchdog_hw->scratch[0] >> 12) & 0xfu) != BADGE_DIAG_HANG) return;
    watchdog_hw->scratch[1] = frame[6];
    watchdog_hw->scratch[2] = frame[5];
}

void __used badge_diag_fault(uint32_t *frame) {
    store(BADGE_DIAG_FAULT, frame[6], frame[5], scb_hw->cfsr);
    watchdog_reboot(0, 0, 1);
    while (true) tight_loop_contents();
}

void __attribute__((naked)) isr_systick(void) {
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "b badge_diag_sample\n");
}

void __attribute__((naked)) isr_hardfault(void) {
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "b badge_diag_fault\n");
}

void __attribute__((noreturn)) badge_panic(const char *fmt, ...) {
    store(BADGE_DIAG_PANIC, (uint32_t)__builtin_return_address(0), 0, (uint32_t)fmt);
    watchdog_reboot(0, 0, 1);
    while (true) tight_loop_contents();
}
