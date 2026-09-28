cmake_minimum_required(VERSION 3.13)

set(projectname picoUxn)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
if(NOT HW_CONFIG)
    set(HW_CONFIG 8 CACHE STRING "Hardware configuration")
endif()
if(NOT USE_HSTX)
    set(USE_HSTX 1 CACHE STRING "Use HSTX display output")
endif()

include("pico_shared/BoardConfigs.cmake")
if(NOT PICO_BOARD)
    set(PICO_BOARD adafruit_fruit_jam CACHE STRING "Board type")
endif()

set(FAMILY rp2040)
include($ENV{PICO_SDK_PATH}/lib/tinyusb/hw/bsp/family_support.cmake)

if(ENABLE_PIO_USB EQUAL 0 AND TARGET tinyusb_common_base)
    get_target_property(_tu_defs tinyusb_common_base INTERFACE_COMPILE_DEFINITIONS)
    if(_tu_defs)
        list(REMOVE_ITEM _tu_defs "CFG_TUH_RPI_PIO_USB=1")
        set_target_properties(tinyusb_common_base PROPERTIES
            INTERFACE_COMPILE_DEFINITIONS "${_tu_defs}")
    endif()
endif()

project(${projectname} C CXX ASM)
pico_sdk_init()

if(ENABLE_PIO_USB EQUAL 1)
    check_and_add_pico_pio_usb_support()
endif()

add_executable(${projectname}
    DefaultSS555.c
    src/main.cpp
    src/uxn_runtime.c
)

pico_set_program_name(${projectname} "picoUxn")
pico_set_program_version(${projectname} "0.1")
pico_enable_stdio_uart(${projectname} 1)
pico_enable_stdio_usb(${projectname} 0)

target_compile_options(${projectname} PRIVATE
    -O3
    -ffunction-sections
    -fdata-sections
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-maybe-uninitialized
)

target_compile_definitions(${projectname} PRIVATE
    PICO_RP2040_USB_DEVICE_ENUMERATION_FIX=1
    DVICONFIG=${DVICONFIG}
    SDCARD_PIN_CS=${SD_CS}
    SDCARD_PIN_SCK=${SD_SCK}
    SDCARD_PIN_MOSI=${SD_MOSI}
    SDCARD_PIN_MISO=${SD_MISO}
    SDCARD_PIO=pio1
    SDCARD_SPI=${SD_SPI}
    NES_PIN_CLK=${NES_CLK}
    NES_PIN_DATA=${NES_DATA}
    NES_PIN_LAT=${NES_LAT}
    NES_PIO=${NES_PIO}
    NES_PIN_CLK_1=${NES_CLK_1}
    NES_PIN_DATA_1=${NES_DATA_1}
    NES_PIN_LAT_1=${NES_LAT_1}
    NES_PIO_1=${NES_PIO_1}
    WII_PIN_SDA=${WII_SDA}
    WII_PIN_SCL=${WII_SCL}
    LED_GPIO_PIN=${LED_GPIO_PIN}
    HW_CONFIG=${HW_CONFIG}
    WIIPAD_I2C=${WIIPAD_I2C}
    USE_I2S_AUDIO=${USE_I2S_AUDIO}
    PICO_AUDIO_I2S_DATA_PIN=${PICO_AUDIO_I2S_DATA_PIN}
    PICO_AUDIO_I2S_CLOCK_PIN_BASE=${PICO_AUDIO_I2S_CLOCK_PIN_BASE}
    PICO_AUDIO_I2S_PIO=${PICO_AUDIO_I2S_PIO}
    PICO_AUDIO_I2S_DMA_IRQ=1
    PICO_AUDIO_I2S_CLOCK_PINS_SWAPPED=${PICO_AUDIO_I2S_CLOCK_PINS_SWAPPED}
    PICO_AUDIO_I2S_RESET_PIN=${PICO_AUDIO_I2S_RESET_PIN}
    PICO_AUDIO_I2S_INTERRUPT_PIN=${PICO_AUDIO_I2S_INTERRUPT_PIN}
    PICO_AUDIO_I2S_INTERRUPT_IS_BUTTON=${PICO_AUDIO_I2S_INTERRUPT_IS_BUTTON}
    CFG_TUH_RPI_PIO_USB=${ENABLE_PIO_USB}
    PIO_USB_USE_PIO=${PIO_USB_USE_PIO}
    PSRAM_CS_PIN=${PSRAM_CS_PIN}
    GPIOHSTXD0=${GPIOHSTXD0}
    GPIOHSTXD1=${GPIOHSTXD1}
    GPIOHSTXD2=${GPIOHSTXD2}
    GPIOHSTXCK=${GPIOHSTXCK}
    GPIOHSTXINVERTED=${GPIOHSTXINVERTED}
    ENABLE_VU_METER=${ENABLE_VU_METER}
    WIIPAD_DELAYED_START=${WIIPAD_DELAYED_START}
    ENABLEDVI=1
    MANTAPAD_DEFAULT_MODE=1
    DI_RING_BUFFER_SIZE=512
    HSTX_AUDIO_DI_HIGH_WATERMARK=504
    I2S_AUDIO_RING_SIZE=2048
    PICO_STACK_SIZE=4096
)

if(DEFINED PICO_DEFAULT_UART_TX_PIN)
    target_compile_definitions(${projectname} PRIVATE
        PICO_DEFAULT_UART_TX_PIN=${PICO_DEFAULT_UART_TX_PIN}
        PICO_DEFAULT_UART_RX_PIN=${PICO_DEFAULT_UART_RX_PIN}
        PICO_DEFAULT_UART=0)
endif()

if(ENABLE_PIO_USB EQUAL 1)
    if(NOT PIO_DP_PLUS_PIN EQUAL -1)
        target_compile_definitions(${projectname} PRIVATE
            PICO_DEFAULT_PIO_USB_DP_PIN=${PIO_DP_PLUS_PIN})
    endif()
    if(DEFINED PIO_DP_PLUS_PIN_2 AND NOT PIO_DP_PLUS_PIN_2 EQUAL -1)
        target_compile_definitions(${projectname} PRIVATE
            PICO_SECOND_PIO_USB_DP_PIN=${PIO_DP_PLUS_PIN_2})
    endif()
    target_link_libraries(${projectname} PRIVATE tinyusb_pico_pio_usb)
endif()

target_include_directories(${projectname} PRIVATE
    src
    pico_lib
    pico_shared
    tusb_xinput
)

target_link_libraries(${projectname} PRIVATE
    pico_stdlib
    pico_multicore
    hardware_dma
    hardware_pio
    hardware_i2c
    hardware_interp
    hardware_timer
    hardware_clocks
    hardware_pwm
    hardware_flash
    dvi
    util
    tinyusb_host
    tinyusb_board
    pico_shared
    xinput_host
)

add_subdirectory(pico_lib/dvi)
add_subdirectory(pico_lib/util)
add_subdirectory(pico_shared)
add_subdirectory(tusb_xinput xinput_host)

target_link_options(${projectname} PRIVATE -Xlinker --print-memory-usage)

if(BUILD_FOR_BOOTLOADER)
    set(_flash_region "${CMAKE_BINARY_DIR}/pico-sdk/src/rp2_common/pico_standard_link/pico_flash_region.ld")
    file(WRITE "${_flash_region}"
        "FLASH(rx) : ORIGIN = 0x10080000, LENGTH = 0xf80000\n")
    target_compile_definitions(${projectname} PRIVATE
        PICO_FLASH_SIZE_BYTES=0x1000000)
    pico_set_uf2_family(${projectname} 0xe48bff59)
endif()

pico_add_extra_outputs(${projectname})
