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
//	Main program, simply calls D_DoomMain high level loop.
//

#include "config.h"

#include <stdlib.h>
#include <stdio.h>

#if !LIB_PICO_STDLIB
#include "SDL.h"
#else
#include "pico/stdlib.h"
#if PICODOOM_CDC_WAIT
#include "pico/stdio_usb.h"
#include "pico/bootrom.h"
#endif
#include "hardware/gpio.h"
#include "pico/sem.h"
#include "pico/multicore.h"
#if PICO_ON_DEVICE
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#endif
#endif
#if USE_PICO_NET
#include "piconet.h"
#endif
#include "doomtype.h"
#include "i_system.h"
#include "i_video.h"
#include "m_argv.h"
#if PICO_DOOM
#include "picodoom.h"
#endif
#if PICO_RP2350
#include "hardware/structs/qmi.h"
#endif

#ifndef PICODOOM_SYS_CLOCK_KHZ
#define PICODOOM_SYS_CLOCK_KHZ 252000
#endif

//
// D_DoomMain()
// Not a globally visible function, just included for source reference,
// calls all startup code, parses command line options.
//

void D_DoomMain (void);

#if PICO_ON_DEVICE
#include "pico/binary_info.h"
#endif

int main(int argc, char **argv)
{
    // save arguments
#if !NO_USE_ARGS
    myargc = argc;
    myargv = argv;
#endif
#if PICO_ON_DEVICE
#if PICO_RP2350
    uint clkdiv = 3;
    uint rxdelay = 2;
    hw_write_masked(
            &qmi_hw->m[0].timing,
            ((clkdiv << QMI_M0_TIMING_CLKDIV_LSB) & QMI_M0_TIMING_CLKDIV_BITS) |
            ((rxdelay << QMI_M0_TIMING_RXDELAY_LSB) & QMI_M0_TIMING_RXDELAY_BITS),
            QMI_M0_TIMING_CLKDIV_BITS | QMI_M0_TIMING_RXDELAY_BITS
    );
#endif
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    busy_wait_us(1000);
    // todo pause? is this the cause of the cold start issue?
    set_sys_clock_khz(PICODOOM_SYS_CLOCK_KHZ, true);
#if !USE_PICO_NET
    // debug ?
//    gpio_debug_pins_init();
#endif
#ifdef PICO_SMPS_MODE_PIN
    gpio_init(PICO_SMPS_MODE_PIN);
    gpio_set_dir(PICO_SMPS_MODE_PIN, GPIO_OUT);
    gpio_put(PICO_SMPS_MODE_PIN, 1);
#endif
#endif
#if LIB_PICO_STDIO
    stdio_init_all();
#endif
#if PICODOOM_CDC_WAIT
    // Diag builds (USB-CDC stdio): hold boot until the host attaches to the
    // serial port; if no host attaches within 10 s, self-reboot to BOOTSEL so
    // a USB-dead build hands itself back for reflashing without the button.
    {
        absolute_time_t cdc_deadline = make_timeout_time_ms(10000);
        while (!stdio_usb_connected() && !time_reached(cdc_deadline)) {
            sleep_ms(100);
        }
        if (!stdio_usb_connected()) {
            reset_usb_boot(0, 0);
        }
        sleep_ms(500); // let the terminal settle before the first prints
        printf("BOOT: rp2350-doom diag (cdc connected=%d)\n", stdio_usb_connected());
    }
#endif
#ifndef PICODOOM_BOOT_HALT
#define PICODOOM_BOOT_HALT 0
#endif
#if PICODOOM_BOOT_HALT == 1
    while (1) { printf("HALT1 after stdio\n"); sleep_ms(1000); }
#endif
#if PICO_ON_DEVICE && defined(PICODOOM_HDMI_DIAG_STAGE) && PICODOOM_HDMI_DIAG_STAGE >= 2
    // Stage 2+3 diagnostics: bring up HDMI immediately so pre-render stalls are visible.
    I_InitGraphics();
#if PICO_DOOM
    hdmi_diag_boot_marker_set(1);
#endif
#if PICODOOM_BOOT_HALT == 2
    while (1) { printf("HALT2 after I_InitGraphics\n"); sleep_ms(1000); }
#endif
#endif
#if PICO_ON_DEVICE && defined(PICODOOM_HDMI_DIAG_STAGE) && PICODOOM_HDMI_DIAG_STAGE == 1
    // Stage 1 diagnostics: bring up HDMI immediately and render bars only.
    while (1) {
        tight_loop_contents();
    }
#endif
#if PICO_BUILD
    I_Init();
#if PICO_DOOM
    hdmi_diag_boot_marker_set(2);
#endif
#endif
#if USE_PICO_NET
    // do init early to set pulls
    piconet_init();
#endif
//!
    // Print the program version and exit.
    //
    if (M_ParmExists("-version") || M_ParmExists("--version")) {
        puts(PACKAGE_STRING);
        exit(0);
    }

#if !NO_USE_ARGS
    M_FindResponseFile();
#endif

    #ifdef SDL_HINT_NO_SIGNAL_HANDLERS
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    #endif

    // start doom
#if PICO_DOOM
    hdmi_diag_boot_marker_set(3);
#endif
    D_DoomMain ();

    return 0;
}
