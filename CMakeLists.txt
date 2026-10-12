cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# AI Passport 独立打印机监控固件
# v1.1: UI 打磨 + 拓竹 logo 开机 + 日志清理
# v1.2: 配网改造(softAP 网页填 WiFi+拓竹账号存 NVS,secrets.h 兜底)
# 产物名 passport_printer.bin
project(passport_printer VERSION 1.2.0)
