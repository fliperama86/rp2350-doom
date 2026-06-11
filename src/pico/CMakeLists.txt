add_library(common_pico INTERFACE)

set(PICODOOM_HDMI_DIAG_STAGE "0" CACHE STRING "HDMI diagnostic stage: 0=normal, 1=color bars, 2=doom base no overlays, 3=normal with deferred vsync work")
set_property(CACHE PICODOOM_HDMI_DIAG_STAGE PROPERTY STRINGS 0 1 2 3)
if (NOT PICODOOM_HDMI_DIAG_STAGE MATCHES "^[0-3]$")
    message(FATAL_ERROR "PICODOOM_HDMI_DIAG_STAGE must be one of: 0,1,2,3")
endif()

set(PICODOOM_SOLID_COLOR "0" CACHE STRING "RGB565 solid color for testing (0 to disable, e.g. 0x07e0)")
set_property(CACHE PICODOOM_SOLID_COLOR PROPERTY STRINGS 0)
if (NOT PICODOOM_SOLID_COLOR MATCHES "^0x[0-9A-Fa-f]+$|^[0-9]+$")
    message(FATAL_ERROR "PICODOOM_SOLID_COLOR must be a hexadecimal (0xRRGG) or decimal value")
endif()

set(PICODOOM_HDMI_DVI_MODE "1" CACHE STRING "HDMI output mode: 1=DVI(video-only), 0=HDMI(data-islands/audio)")
set_property(CACHE PICODOOM_HDMI_DVI_MODE PROPERTY STRINGS 0 1)
if (NOT PICODOOM_HDMI_DVI_MODE MATCHES "^[01]$")
    message(FATAL_ERROR "PICODOOM_HDMI_DVI_MODE must be 0 or 1")
endif()

set(PICODOOM_HDMI_240P "0" CACHE STRING "Use pico_hdmi's 320x240/240p timing instead of 640x480")
set_property(CACHE PICODOOM_HDMI_240P PROPERTY STRINGS 0 1)
if (NOT PICODOOM_HDMI_240P MATCHES "^[01]$")
    message(FATAL_ERROR "PICODOOM_HDMI_240P must be 0 or 1")
endif()

set(PICODOOM_HDMI_PREPARED_SCANLINES "0" CACHE STRING "Preconvert each published frame to a stable RGB565 source frame before HDMI scanout")
set_property(CACHE PICODOOM_HDMI_PREPARED_SCANLINES PROPERTY STRINGS 0 1)
if (NOT PICODOOM_HDMI_PREPARED_SCANLINES MATCHES "^[01]$")
    message(FATAL_ERROR "PICODOOM_HDMI_PREPARED_SCANLINES must be 0 or 1")
endif()

set(PICODOOM_HDMI_FREEZE_RGB565_FRAME "0" CACHE STRING "Diagnostic: build the RGB565 HDMI source frame once, then keep normal render pacing without updating it")
set_property(CACHE PICODOOM_HDMI_FREEZE_RGB565_FRAME PROPERTY STRINGS 0 1)
if (NOT PICODOOM_HDMI_FREEZE_RGB565_FRAME MATCHES "^[01]$")
    message(FATAL_ERROR "PICODOOM_HDMI_FREEZE_RGB565_FRAME must be 0 or 1")
endif()

set(PICODOOM_HDMI_NATIVE_POINTER_TEST "0" CACHE STRING "Diagnostic: scan out the cached native RGB565 frame directly without 480p horizontal expansion")
set_property(CACHE PICODOOM_HDMI_NATIVE_POINTER_TEST PROPERTY STRINGS 0 1)
if (NOT PICODOOM_HDMI_NATIVE_POINTER_TEST MATCHES "^[01]$")
    message(FATAL_ERROR "PICODOOM_HDMI_NATIVE_POINTER_TEST must be 0 or 1")
endif()

set(PICODOOM_HDMI_SOLID_POINTER_TEST "0" CACHE STRING "Diagnostic: scan out a solid line through the prepared pointer callback")
set_property(CACHE PICODOOM_HDMI_SOLID_POINTER_TEST PROPERTY STRINGS 0 1)
if (NOT PICODOOM_HDMI_SOLID_POINTER_TEST MATCHES "^[01]$")
    message(FATAL_ERROR "PICODOOM_HDMI_SOLID_POINTER_TEST must be 0 or 1")
endif()

set(PICODOOM_HDMI_LINE_RING "0" CACHE STRING "Pre-expand RGB565 lines outside the HDMI scanline IRQ and scan them out by pointer")
set_property(CACHE PICODOOM_HDMI_LINE_RING PROPERTY STRINGS 0 1)
if (NOT PICODOOM_HDMI_LINE_RING MATCHES "^[01]$")
    message(FATAL_ERROR "PICODOOM_HDMI_LINE_RING must be 0 or 1")
endif()

set(PICODOOM_HDMI_LINE_RING_DIRECT "0" CACHE STRING "Prepare HDMI line-ring entries directly from Doom's indexed framebuffer")
set_property(CACHE PICODOOM_HDMI_LINE_RING_DIRECT PROPERTY STRINGS 0 1)
if (NOT PICODOOM_HDMI_LINE_RING_DIRECT MATCHES "^[01]$")
    message(FATAL_ERROR "PICODOOM_HDMI_LINE_RING_DIRECT must be 0 or 1")
endif()

set(PICODOOM_HDMI_LINE_RING_SIZE "32" CACHE STRING "Number of 640-pixel RGB565 lines in the HDMI pre-expanded line ring")
if (NOT PICODOOM_HDMI_LINE_RING_SIZE MATCHES "^[0-9]+$")
    message(FATAL_ERROR "PICODOOM_HDMI_LINE_RING_SIZE must be a positive integer")
endif()

set(PICODOOM_HDMI_LINE_RING_PREPARE "1" CACHE STRING "Diagnostic: enable background preparation of pre-expanded HDMI line-ring entries")
set_property(CACHE PICODOOM_HDMI_LINE_RING_PREPARE PROPERTY STRINGS 0 1)
if (NOT PICODOOM_HDMI_LINE_RING_PREPARE MATCHES "^[01]$")
    message(FATAL_ERROR "PICODOOM_HDMI_LINE_RING_PREPARE must be 0 or 1")
endif()

set(PICODOOM_HDMI_LINE_RING_VBLANK_ONLY "1" CACHE STRING "Diagnostic: service the HDMI line ring only during vertical blank")
set_property(CACHE PICODOOM_HDMI_LINE_RING_VBLANK_ONLY PROPERTY STRINGS 0 1)
if (NOT PICODOOM_HDMI_LINE_RING_VBLANK_ONLY MATCHES "^[01]$")
    message(FATAL_ERROR "PICODOOM_HDMI_LINE_RING_VBLANK_ONLY must be 0 or 1")
endif()

set(PICODOOM_HDMI_CMDLIST "0" CACHE STRING "Use the per-frame DMA command-list HSTX scanout (picodvi/fruitjam-doom style, DVI only, no per-line ISR) instead of pico_hdmi")
set_property(CACHE PICODOOM_HDMI_CMDLIST PROPERTY STRINGS 0 1)
if (NOT PICODOOM_HDMI_CMDLIST MATCHES "^[01]$")
    message(FATAL_ERROR "PICODOOM_HDMI_CMDLIST must be 0 or 1")
endif()

set(PICODOOM_HDMI_LITE "0" CACHE STRING "Use the pico_hdmi transport with pre-composed line headers + native pixel pointers (ISR does pointer swaps only): stable video AND HDMI audio")
set_property(CACHE PICODOOM_HDMI_LITE PROPERTY STRINGS 0 1)
if (NOT PICODOOM_HDMI_LITE MATCHES "^[01]$")
    message(FATAL_ERROR "PICODOOM_HDMI_LITE must be 0 or 1")
endif()

set(PICODOOM_BOOT_HALT "0" CACHE STRING "Diag: halt boot at stage N with a print loop (1=after stdio, 2=after I_InitGraphics) to bisect USB-killing init steps")
if (NOT PICODOOM_BOOT_HALT MATCHES "^[0-9]$")
    message(FATAL_ERROR "PICODOOM_BOOT_HALT must be 0-9")
endif()

set(PICODOOM_NO_MUSIC "0" CACHE STRING "Diagnostic gate: never install the OPL music generator (SFX-only mixer); song registration and level transitions still run")
set_property(CACHE PICODOOM_NO_MUSIC PROPERTY STRINGS 0 1)
if (NOT PICODOOM_NO_MUSIC MATCHES "^[01]$")
    message(FATAL_ERROR "PICODOOM_NO_MUSIC must be 0 or 1")
endif()

set(PICODOOM_HDMI_AUDIO_TEST_TONE "0" CACHE STRING "Diagnostic: 1=feed a 1 kHz square wave into the HDMI audio islands (bypasses the game mixer), 2=bake the tone statically into the ring at init (no dynamic island writes at all)")
set_property(CACHE PICODOOM_HDMI_AUDIO_TEST_TONE PROPERTY STRINGS 0 1 2 3)
if (NOT PICODOOM_HDMI_AUDIO_TEST_TONE MATCHES "^[0-3]$")
    message(FATAL_ERROR "PICODOOM_HDMI_AUDIO_TEST_TONE must be 0-3 (3 = pure sine)")
endif()

set(PICODOOM_HDMI_FIFO_PROBE "0" CACHE STRING "Diagnostic: sample the HSTX FIFO level/EMPTY per active scanline and report starvation over UART")
set_property(CACHE PICODOOM_HDMI_FIFO_PROBE PROPERTY STRINGS 0 1)
if (NOT PICODOOM_HDMI_FIFO_PROBE MATCHES "^[01]$")
    message(FATAL_ERROR "PICODOOM_HDMI_FIFO_PROBE must be 0 or 1")
endif()

set(PICODOOM_SKIP_WIPES "0" CACHE STRING "Skip Pico Doom melt wipes so state transitions cannot block on display-side wipe progress")
set_property(CACHE PICODOOM_SKIP_WIPES PROPERTY STRINGS 0 1)
if (NOT PICODOOM_SKIP_WIPES MATCHES "^[01]$")
    message(FATAL_ERROR "PICODOOM_SKIP_WIPES must be 0 or 1")
endif()

set(PICODOOM_BOOT_TO_E1M1 "0" CACHE STRING "Start directly on E1M1 instead of the title/demo loop")
set_property(CACHE PICODOOM_BOOT_TO_E1M1 PROPERTY STRINGS 0 1)
if (NOT PICODOOM_BOOT_TO_E1M1 MATCHES "^[01]$")
    message(FATAL_ERROR "PICODOOM_BOOT_TO_E1M1 must be 0 or 1")
endif()

set(PICODOOM_FORCE_SOLID_SCANOUT "0" CACHE STRING "Force the HDMI scanline callback to output PICODOOM_SOLID_COLOR while Doom continues running")
set_property(CACHE PICODOOM_FORCE_SOLID_SCANOUT PROPERTY STRINGS 0 1)
if (NOT PICODOOM_FORCE_SOLID_SCANOUT MATCHES "^[01]$")
    message(FATAL_ERROR "PICODOOM_FORCE_SOLID_SCANOUT must be 0 or 1")
endif()

set(PICODOOM_PUBLISH_FIRST_FRAME_ONLY "0" CACHE STRING "Publish the first rendered frame, then keep rendering without display-side handoff")
set_property(CACHE PICODOOM_PUBLISH_FIRST_FRAME_ONLY PROPERTY STRINGS 0 1)
if (NOT PICODOOM_PUBLISH_FIRST_FRAME_ONLY MATCHES "^[01]$")
    message(FATAL_ERROR "PICODOOM_PUBLISH_FIRST_FRAME_ONLY must be 0 or 1")
endif()

set(PICODOOM_WAIT_AFTER_FRAME_PUBLISH "0" CACHE STRING "After publishing a frame, wait until HDMI has consumed and prepared it")
set_property(CACHE PICODOOM_WAIT_AFTER_FRAME_PUBLISH PROPERTY STRINGS 0 1)
if (NOT PICODOOM_WAIT_AFTER_FRAME_PUBLISH MATCHES "^[01]$")
    message(FATAL_ERROR "PICODOOM_WAIT_AFTER_FRAME_PUBLISH must be 0 or 1")
endif()

set(PICODOOM_IDLE_AFTER_FIRST_DISPLAY "0" CACHE STRING "After the first complete display frame, idle Core 0 with sound/USB service only")
set_property(CACHE PICODOOM_IDLE_AFTER_FIRST_DISPLAY PROPERTY STRINGS 0 1)
if (NOT PICODOOM_IDLE_AFTER_FIRST_DISPLAY MATCHES "^[01]$")
    message(FATAL_ERROR "PICODOOM_IDLE_AFTER_FIRST_DISPLAY must be 0 or 1")
endif()

set(PICODOOM_IDLE_AFTER_FIRST_DISPLAY_LOAD "0" CACHE STRING "Diagnostic Core 0 load after first displayed frame: 0=quiet service loop, 1=scratch-RAM ALU burn, 2=scratch-code main-RAM sweep, 3=scratch-code WHX XIP read sweep, 4=mode-3 one-read/no-load control")
set_property(CACHE PICODOOM_IDLE_AFTER_FIRST_DISPLAY_LOAD PROPERTY STRINGS 0 1 2 3 4)
if (NOT PICODOOM_IDLE_AFTER_FIRST_DISPLAY_LOAD MATCHES "^[0-4]$")
    message(FATAL_ERROR "PICODOOM_IDLE_AFTER_FIRST_DISPLAY_LOAD must be 0, 1, 2, 3, or 4")
endif()

set(PICODOOM_IDLE_XIP_LOAD_GAP "0" CACHE STRING "Diagnostic ALU work inserted after each XIP load word in idle XIP-load mode")
if (NOT PICODOOM_IDLE_XIP_LOAD_GAP MATCHES "^[0-9]+$")
    message(FATAL_ERROR "PICODOOM_IDLE_XIP_LOAD_GAP must be a non-negative integer")
endif()

set(PICODOOM_FREEZE_POINT "0" CACHE STRING "Freeze Core 0 at a diagnostic checkpoint")
set_property(CACHE PICODOOM_FREEZE_POINT PROPERTY STRINGS 0 1 2 3 4 5 6 7 8 9 10 11 12 13)
if (NOT PICODOOM_FREEZE_POINT MATCHES "^([0-9]|1[0-3])$")
    message(FATAL_ERROR "PICODOOM_FREEZE_POINT must be 0 through 13")
endif()

set(PICODOOM_RENDER_THROTTLE_US "0" CACHE STRING "Diagnostic Core 0 render throttle delay in microseconds")
if (NOT PICODOOM_RENDER_THROTTLE_US MATCHES "^[0-9]+$")
    message(FATAL_ERROR "PICODOOM_RENDER_THROTTLE_US must be a non-negative integer")
endif()

set(PICODOOM_FRAME_CAP_FPS "0" CACHE STRING "Diagnostic frame cap in frames per second; 0 disables the cap")
if (NOT PICODOOM_FRAME_CAP_FPS MATCHES "^[0-9]+$")
    message(FATAL_ERROR "PICODOOM_FRAME_CAP_FPS must be a non-negative integer")
endif()

set(PICODOOM_SYS_CLOCK_KHZ "252000" CACHE STRING "Pico system clock in kHz")
if (NOT PICODOOM_SYS_CLOCK_KHZ MATCHES "^[0-9]+$")
    message(FATAL_ERROR "PICODOOM_SYS_CLOCK_KHZ must be a positive integer")
endif()

set(PICODOOM_HDMI_HSTX_CLK_DIV "2" CACHE STRING "HSTX clock divider used by pico_hdmi")
if (NOT PICODOOM_HDMI_HSTX_CLK_DIV MATCHES "^[0-9]+$")
    message(FATAL_ERROR "PICODOOM_HDMI_HSTX_CLK_DIV must be a positive integer")
endif()

target_sources(common_pico INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/i_glob.c
        ${CMAKE_CURRENT_LIST_DIR}/i_input.c
        ${CMAKE_CURRENT_LIST_DIR}/i_system.c
        ${CMAKE_CURRENT_LIST_DIR}/piconet.c
        ${CMAKE_CURRENT_LIST_DIR}/i_timer.c
        ${CMAKE_CURRENT_LIST_DIR}/i_video.c
        ${CMAKE_CURRENT_LIST_DIR}/stubs.c

        ${CMAKE_CURRENT_LIST_DIR}/i_picosound.c
)
if (PICO_ON_DEVICE)
    target_sources(common_pico INTERFACE
            ${CMAKE_CURRENT_LIST_DIR}/blit.S
            ${CMAKE_CURRENT_LIST_DIR}/hstx_cmdlist.c
            ${CMAKE_CURRENT_LIST_DIR}/picoflash.c
            )
    if (NOT PICODOOM_TEST_NO_ZONE_MALLOC)
        pico_wrap_function(common_pico malloc)
        pico_wrap_function(common_pico calloc)
        pico_wrap_function(common_pico free)
        target_compile_definitions(common_pico INTERFACE
                PICO_HEAP_SIZE=0
                USE_ZONE_FOR_MALLOC=1
        )
    endif()
    target_link_libraries(common_pico INTERFACE  hardware_i2c)
endif()
target_include_directories(common_pico INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/..
        ${CMAKE_CURRENT_LIST_DIR}/../../textscreen
        )

target_compile_definitions(common_pico INTERFACE
        NO_USE_MOUSE=1
        PICO_USE_SW_SPIN_LOCKS=0
        PICO_USE_STACK_GUARDS=0 # todo we can actually use these sensibly, but right now we do overflow!
        MODE_HSTX_CLK_DIV=${PICODOOM_HDMI_HSTX_CLK_DIV}
        PICODOOM_SYS_CLOCK_KHZ=${PICODOOM_SYS_CLOCK_KHZ}
        PICODOOM_HDMI_DIAG_STAGE=${PICODOOM_HDMI_DIAG_STAGE}
        PICODOOM_HDMI_DVI_MODE=${PICODOOM_HDMI_DVI_MODE}
        PICODOOM_HDMI_240P=${PICODOOM_HDMI_240P}
        PICODOOM_HDMI_PREPARED_SCANLINES=${PICODOOM_HDMI_PREPARED_SCANLINES}
        PICODOOM_HDMI_FREEZE_RGB565_FRAME=${PICODOOM_HDMI_FREEZE_RGB565_FRAME}
        PICODOOM_HDMI_NATIVE_POINTER_TEST=${PICODOOM_HDMI_NATIVE_POINTER_TEST}
        PICODOOM_HDMI_SOLID_POINTER_TEST=${PICODOOM_HDMI_SOLID_POINTER_TEST}
        PICODOOM_HDMI_LINE_RING=${PICODOOM_HDMI_LINE_RING}
        PICODOOM_HDMI_LINE_RING_DIRECT=${PICODOOM_HDMI_LINE_RING_DIRECT}
        PICODOOM_HDMI_LINE_RING_SIZE=${PICODOOM_HDMI_LINE_RING_SIZE}
        PICODOOM_HDMI_LINE_RING_PREPARE=${PICODOOM_HDMI_LINE_RING_PREPARE}
        PICODOOM_HDMI_LINE_RING_VBLANK_ONLY=${PICODOOM_HDMI_LINE_RING_VBLANK_ONLY}
        PICODOOM_HDMI_CMDLIST=${PICODOOM_HDMI_CMDLIST}
        PICODOOM_HDMI_LITE=${PICODOOM_HDMI_LITE}
        $<$<BOOL:${PICODOOM_HDMI_LITE}>:PICO_HDMI_PRECOMPOSED_ACTIVE_LINES=1>
        PICODOOM_BOOT_HALT=${PICODOOM_BOOT_HALT}
        PICODOOM_NO_MUSIC=${PICODOOM_NO_MUSIC}
        PICODOOM_HDMI_AUDIO_TEST_TONE=${PICODOOM_HDMI_AUDIO_TEST_TONE}
        $<$<BOOL:${PICODOOM_HDMI_240P}>:VIDEO_MODE_320x240=1>
        PICODOOM_SOLID_COLOR=${PICODOOM_SOLID_COLOR}
        PICODOOM_HDMI_FIFO_PROBE=${PICODOOM_HDMI_FIFO_PROBE}
        PICODOOM_SKIP_WIPES=${PICODOOM_SKIP_WIPES}
        PICODOOM_BOOT_TO_E1M1=${PICODOOM_BOOT_TO_E1M1}
        PICODOOM_FORCE_SOLID_SCANOUT=${PICODOOM_FORCE_SOLID_SCANOUT}
        PICODOOM_PUBLISH_FIRST_FRAME_ONLY=${PICODOOM_PUBLISH_FIRST_FRAME_ONLY}
        PICODOOM_WAIT_AFTER_FRAME_PUBLISH=${PICODOOM_WAIT_AFTER_FRAME_PUBLISH}
        PICODOOM_IDLE_AFTER_FIRST_DISPLAY=${PICODOOM_IDLE_AFTER_FIRST_DISPLAY}
        PICODOOM_IDLE_AFTER_FIRST_DISPLAY_LOAD=${PICODOOM_IDLE_AFTER_FIRST_DISPLAY_LOAD}
        PICODOOM_IDLE_XIP_LOAD_GAP=${PICODOOM_IDLE_XIP_LOAD_GAP}
        PICODOOM_FREEZE_POINT=${PICODOOM_FREEZE_POINT}
        PICODOOM_RENDER_THROTTLE_US=${PICODOOM_RENDER_THROTTLE_US}
        PICODOOM_FRAME_CAP_FPS=${PICODOOM_FRAME_CAP_FPS}
)

target_link_libraries(common_pico INTERFACE pico_stdlib pico_multicore pico_hdmi)

add_library(pico_cd INTERFACE)
if (TARGET tinyusb_host)
    target_link_libraries(pico_cd INTERFACE tinyusb_host)
endif()
