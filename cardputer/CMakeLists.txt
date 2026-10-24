# Sapphire satellite firmware for the M5Stack Cardputer ADV
# (Stamp-S3A = ESP32-S3FN8: 8 MB flash, NO PSRAM). Build: see README.md (ESP-IDF v5.5).
cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "0.1.0")                  # said in /health as "firmware"
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sapphire-cardputer)
