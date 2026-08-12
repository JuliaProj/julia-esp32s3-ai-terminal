cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_SOURCE_DIR}/waveshare_demo/ESP-IDF/ESP32-S3-LCD-1.85-Test/components/lvgl__lvgl"
    "${CMAKE_SOURCE_DIR}/waveshare_demo/ESP-IDF/ESP32-S3-LCD-1.85-Test/components/espressif__esp-dsp"
    "${CMAKE_SOURCE_DIR}/waveshare_demo/ESP-IDF/ESP32-S3-LCD-1.85-Test/components/espressif__esp-sr")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(julia-ai LANGUAGES C CXX)
