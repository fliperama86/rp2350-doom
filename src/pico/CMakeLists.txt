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
            ${CMAKE_CURRENT_LIST_DIR}/picoflash.c
            )
    pico_wrap_function(common_pico malloc)
    pico_wrap_function(common_pico calloc)
    pico_wrap_function(common_pico free)
    target_compile_definitions(common_pico INTERFACE
            PICO_HEAP_SIZE=0
            USE_ZONE_FOR_MALLOC=1
    )
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
        MODE_HSTX_CLK_DIV=2
        PICODOOM_HDMI_DIAG_STAGE=${PICODOOM_HDMI_DIAG_STAGE}
        PICODOOM_HDMI_DVI_MODE=${PICODOOM_HDMI_DVI_MODE}
        PICODOOM_SOLID_COLOR=${PICODOOM_SOLID_COLOR}
)

target_link_libraries(common_pico INTERFACE pico_stdlib pico_multicore pico_hdmi)

add_library(pico_cd INTERFACE)
if (TARGET tinyusb_host)
    target_link_libraries(pico_cd INTERFACE tinyusb_host)
endif()
