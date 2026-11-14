#include "spirit_external_assets.h"

#include "esp_log.h"
#include "esp_partition.h"

void spirit_external_init(void)
{
    static bool attempted;
    static esp_partition_mmap_handle_t mapping;
    if (attempted) return;
    attempted = true;
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
    size_t bytes = spirit_external_pack_bytes();
    if (!partition || bytes > partition->size) {
        ESP_LOGW("spirit_pack", "Character resource partition is absent or too small");
        return;
    }
    const void *address = NULL;
    if (esp_partition_mmap(partition, 0, bytes, ESP_PARTITION_MMAP_DATA,
                           &address, &mapping) != ESP_OK) {
        ESP_LOGW("spirit_pack", "Character resource mapping failed");
        return;
    }
    if (!spirit_external_attach(address, bytes)) {
        esp_partition_munmap(mapping);
        ESP_LOGW("spirit_pack", "Missing, mismatched or damaged character resources; keeping original four spirits");
        return;
    }
    ESP_LOGI("spirit_pack", "Validated Spirit resource schema 1: %u bytes", (unsigned)bytes);
}
