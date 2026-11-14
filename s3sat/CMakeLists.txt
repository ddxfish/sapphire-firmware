# Sapphire's S3 satellites: one program, two boards. `build.sh pendant` or `build.sh ai-speaker-mic`
# sets BOARD; main/boards/<BOARD>.c and .h (and the files under main/boards/<BOARD>/) are the only code
# that differs, main/boards/<BOARD>.defaults the only configuration. 1.0.0 follows the pendant (0.6.1)
# and the AI speaker (0.9.5) it replaces (2026-11-12: the two trees had drifted 4130 lines apart).
# Build:  see README.md (needs ESP-IDF v5.5).
cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "1.0.0")                  # said in /health as "firmware"
set(BOARD "pendant" CACHE STRING "the board: a name under main/boards/")
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/main/boards/${BOARD}.c")
    message(FATAL_ERROR "no board named '${BOARD}' under main/boards/")
endif()
# the board's own configuration over the shared one (its console, its camera's sensors and bus)
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/main/boards/${BOARD}.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sapphire-${BOARD})                # the OTA checks this name: it must stay sapphire-pendant / sapphire-ai-speaker-mic
# LVGL's heap lives in main/lv_mem_psram.c and nothing of ours names those functions, so
# the linker has passed our archive by the time LVGL's asks for them. A -u ahead of every
# archive (link OPTIONS come before the libraries) pulls that object in first.
target_link_options(${CMAKE_PROJECT_NAME}.elf PRIVATE "-u" "lv_mem_init")
