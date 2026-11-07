idf_component_register(
    SRCS "main.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "app_wifi.c"
         "app_wifi_menu.c"
         "app_mqtt.c"
         "app_notify.c"
         "app_notify_transition.c"
         "app_boot.c"
         "app_printer.c"
         "app_config.c"
         "app_camera.c"
         "app_camera_config.c"
         "camera_geometry.c"
         "app_provision.c"
         "hms_codes.c"
         "logo_bambu.c"
         "font_zh14.c"
         "font_zh20.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_wifi nvs_flash esp_event esp_netif esp_timer esp_pm esp_driver_usb_serial_jtag esp_http_server esp_http_client mqtt json mbedtls esp-tls
)

# 固件版本宏(取自 CMake project VERSION,供设备页显示)
target_compile_definitions(${COMPONENT_LIB} PUBLIC FW_VERSION="${PROJECT_VERSION}")
if(CONFIG_CAMERA_GEOMETRY_SELFTEST)
    target_sources(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../tools/camera_geometry_selftest.c")
endif()
option(CAMERA_DIAGNOSTIC_START "Open camera automatically for on-device smoke tests" OFF)
if(CAMERA_DIAGNOSTIC_START)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CAMERA_DIAGNOSTIC_START=1)
endif()
option(NOTIFY_TRANSITION_SELFTEST "Test notification transitions at boot without playing audio" OFF)
if(NOTIFY_TRANSITION_SELFTEST)
    target_sources(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../tools/notify_transition_selftest.c")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NOTIFY_TRANSITION_SELFTEST=1)
endif()
