cmake_minimum_required(VERSION 3.16)
# One program, two boards: `build.sh cyd28` or `build.sh cyd35` sets BOARD, and
# main/boards/<BOARD>.c and .h are the only files that differ. 0.5.0 follows
# both the 2.8" reader (0.1.0) and the 3.5" pocket (0.4.5) it replaces.
set(PROJECT_VER "0.5.0")
set(BOARD "cyd28" CACHE STRING "the board: a name under main/boards/")
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/main/boards/${BOARD}.c")
    message(FATAL_ERROR "no board named '${BOARD}' under main/boards/")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sapphire-${BOARD})
