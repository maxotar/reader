cmake_minimum_required(VERSION 3.16)

set(COMPONENTS 
    main 
    esp_lcd 
    lvgl 
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
    xtensa 
    newlib
)

# sccache integration
find_program(SCCACHE sccache)
if(SCCACHE)
    message(STATUS "Using sccache: ${SCCACHE}")
    set(CMAKE_C_COMPILER_LAUNCHER   ${SCCACHE})
    set(CMAKE_CXX_COMPILER_LAUNCHER ${SCCACHE})
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(app)
