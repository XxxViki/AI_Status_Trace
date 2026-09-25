# AI Status —— ESP32-C3 智能硬件（AI 状态红绿灯）
# 顶层 CMakeLists：ESP-IDF 工程入口，include 官方构建系统后声明工程名
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ai_status)
