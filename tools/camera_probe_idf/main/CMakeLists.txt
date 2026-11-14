idf_component_register(
    SRCS "camera_probe.c" "${REPO_ROOT}/components/bsp/src/bsp_display.c"
    INCLUDE_DIRS "${REPO_ROOT}/components/bsp/include" "${PROBE_PRIVATE_DIR}"
    REQUIRES driver esp_lcd esp_wifi esp_event esp_netif nvs_flash esp_timer
             esp-tls mbedtls json esp_rom
)
