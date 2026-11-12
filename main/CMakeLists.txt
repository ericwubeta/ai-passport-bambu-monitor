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
         "app_config.c"
         "app_provision.c"
         "hms_codes.c"
         "logo_bambu.c"
         "font_zh14.c"
         "font_zh20.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_wifi nvs_flash esp_event esp_netif esp_timer esp_pm esp_driver_usb_serial_jtag esp_http_server esp_http_client mqtt json mbedtls
)

# 从 ESP-IDF 构建属性读取版本，避免组件作用域中的 PROJECT_VERSION 为空。
idf_build_get_property(printpet_firmware_version PROJECT_VER)
target_compile_definitions(${COMPONENT_LIB} PUBLIC FW_VERSION="${printpet_firmware_version}")
