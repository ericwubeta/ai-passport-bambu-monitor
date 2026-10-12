idf_component_register(
    SRCS "main.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "app_wifi.c"
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
    REQUIRES bsp esp_wifi nvs_flash esp_event esp_netif esp_timer esp_http_server mqtt json mbedtls
)

# 固件版本宏(取自 CMake project VERSION,供设备页显示)
target_compile_definitions(${COMPONENT_LIB} PUBLIC FW_VERSION="${PROJECT_VERSION}")
