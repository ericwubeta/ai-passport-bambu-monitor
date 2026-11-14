idf_component_register(
    SRCS "main.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "app_wifi.c"
         "app_wifi_menu.c"
         "app_mqtt.c"
         "app_notify.c"
         "app_boot.c"
         "app_printer.c"
         "app_spirit_logic.c"
         "app_spirit_view.c"
         "app_spirit_picker.c"
         "app_spirit_skins.c"
         "app_settings.c"
         "app_telemetry.c"
         "nozzle_schema_decode.c"
         "spirit_bindings.c"
         "spirit_dispatch.c"
         "spirit_adpcm.c"
         "spirit_external_assets.c"
         "../assets/images/print-spirit/v220-external-v01/spirit_external_generated.c"
         "spirit_event_logic.c"
         "../assets/images/print-spirit/v212-i4-s112-v01/spirit_device_assets.c"
         "../assets/images/print-spirit/v218-voice-v03/spirit_voice_assets.c"
         "../assets/images/print-spirit/v218-page-voice-v03/spirit_page_voice_assets.c"
         "app_config.c"
         "app_provision.c"
         "hms_codes.c"
         "logo_bambu.c"
         "../assets/fonts/v220/font_zh14.c"
         "../assets/fonts/v220/font_zh20.c"
    INCLUDE_DIRS "." "../assets/images/print-spirit/v220-external-v01" "../assets/images/print-spirit/v212-i4-s112-v01" "../assets/images/print-spirit/v218-voice-v03" "../assets/images/print-spirit/v218-page-voice-v03"
    REQUIRES esp_partition bsp esp_wifi nvs_flash esp_event esp_netif esp_timer esp_pm esp_driver_usb_serial_jtag esp_http_server esp_http_client mqtt json mbedtls
)

# 固件版本宏(设备页标题显示)。ESP-IDF 在组件作用域里拿不到 PROJECT_VERSION
# (2.1.6 及更早版本编译出来的 FW_VERSION 是空串,设备页只显示 “X2D v”),
# 所以这里写死版本号;改顶层 project() VERSION 时必须同步改这一行。
target_compile_definitions(${COMPONENT_LIB} PUBLIC FW_VERSION="2.0.1")
