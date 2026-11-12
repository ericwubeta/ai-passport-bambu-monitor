cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# PrintPet Core — read-only companion firmware for FoloToy AI Passport hardware.
# v1.1: UI 打磨 + 拓竹 logo 开机 + 日志清理
# v1.2: 配网改造(softAP 网页填 WiFi+拓竹账号存 NVS,secrets.h 兜底)
# v1.3: WiFi 多组记忆(3 组)+扫描点选配网+设备端手动切网+状态条显示当前 WiFi
# v1.4: 首次配置全程手机完成(网页即时连 WiFi + 拓竹短信登录自动取 Token/UID/打印机)
# v1.5: 省电(空闲熄屏 + Wi-Fi modem sleep + 音频按需 + 自动 light sleep)+ 云端大报文本地修复
# 产物名 passport_printer.bin
# v1.5.1: Core read-only cleanup; historical v1.5.0 tag remains unchanged.
project(passport_printer VERSION 1.5.1)
