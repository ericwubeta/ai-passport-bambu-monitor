cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# PrintPet Spirit — third-party firmware for FoloToy AI Passport hardware.
# v1.1: UI 打磨 + 拓竹 logo 开机 + 日志清理
# v1.2: 配网改造(softAP 网页填 WiFi+拓竹账号存 NVS,secrets.h 兜底)
# v1.3: WiFi 多组记忆(3 组)+扫描点选配网+设备端手动切网+状态条显示当前 WiFi
# v1.4: 首次配置全程手机完成(网页即时连 WiFi + 拓竹短信登录自动取 Token/UID/打印机)
# v1.5: 省电(空闲熄屏 + Wi-Fi modem sleep + 音频按需 + 自动 light sleep)+ 云端大报文本地修复
# 产物名 passport_printer.bin（保留构建/升级兼容标识，不作为产品名）。
# v2.0: native print spirit homepage; state-driven playback (device validation pending).
# v2.0.1: four selectable spirits, persisted preference and read-only printer pages.
# v2.0.2: defer all menu navigation to LVGL; swallow the whole wake gesture.
# v2.1.1: reduce repeated status-bar/empty-state text and fold the OK affordance
#         into the value line; remove the stale per-tick snapshot hash.
# v2.1.2: parse get_version product_name for model display; 112x112 square
#         sprite canvas removes empty vertical margins and enlarges the character.
# v2.1.3: model-aware nozzle layout, AMS info below the character, screen-level
#         speech bubble visible on every detail page, resume greeting.
# v2.1.4: one compact nozzle page for 1-4 heads; 2x2 AMS grid; device model/version
#         + RSSI bars at the bottom; per-page entry dialogue and ETA chatter.
# v2.1.5: nozzles ordered by the active head first (喷头1) then ascending ID;
#         per-role spoken page hints from the frozen voice references (8kHz).
# v2.1.6: menu gains a 设置 page for volume and backlight, persisted in NVS.
# v2.1.7: device page shows the real version (FW_VERSION was empty); each
#         detail page gets its own role line and clip; event clips re-encoded
#         after silence trimming to pay for the extra page audio.
# v2.1.8: alternate page phrase per page/role, longer pre-roll on event clips,
#         home chatter with three wordings; version moved off the model title.
# v2.2.0: six spirits, versioned read-only external character resources.
project(passport_printer VERSION 2.0.1)
