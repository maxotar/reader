cmake_minimum_required(VERSION 3.16)

# sccache integration
find_program(SCCACHE sccache)
if(SCCACHE)
    message(STATUS "Using sccache: ${SCCACHE}")
    set(CMAKE_C_COMPILER_LAUNCHER   ${SCCACHE})
    set(CMAKE_CXX_COMPILER_LAUNCHER ${SCCACHE})
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(app)
