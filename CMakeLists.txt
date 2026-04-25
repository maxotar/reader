cmake_minimum_required(VERSION 3.16)

# sccache integration
find_program(SCCACHE sccache)
if(SCCACHE)
    message(STATUS "Using sccache: ${SCCACHE}")
    set(CMAKE_C_COMPILER_LAUNCHER ${SCCACHE})
    set(CMAKE_CXX_COMPILER_LAUNCHER ${SCCACHE})
endif()

set(COMPONENTS
    main
    esp_lcd
    esp_psram
    freertos
    esp_timer
    driver
    esp_hw_support
    heap
    log
    soc
    hal
    esp_rom
    esp_common
    esp_system
    spi_flash
    spiffs
    nvs_flash
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(app)