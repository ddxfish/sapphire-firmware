# Sapphire pendant: the Elecrow AI Camera board as her satellite on a cord.
# Build:  see README.md (needs ESP-IDF v5.5).
cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "0.5.2")                  # said in /health as "firmware"
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sapphire-pendant)
# LVGL's heap lives in main/lv_mem_psram.c and nothing of ours names those functions, so
# the linker has passed our archive by the time LVGL's asks for them. A -u ahead of every
# archive (link OPTIONS come before the libraries) pulls that object in first.
target_link_options(${CMAKE_PROJECT_NAME}.elf PRIVATE "-u" "lv_mem_init")
