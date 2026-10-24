cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# AI Passport 独立打印机监控固件
# v1.1: UI 打磨 + 拓竹 logo 开机 + 日志清理
# v1.2: 配网改造(softAP 网页填 WiFi+拓竹账号存 NVS,secrets.h 兜底)
# v1.3: WiFi 多组记忆(3 组)+扫描点选配网+设备端手动切网+状态条显示当前 WiFi
# v1.4: 首次配置全程手机完成(网页即时连 WiFi + 拓竹短信登录自动取 Token/UID/打印机)
# 产物名 passport_printer.bin
project(passport_printer VERSION 1.4.0)
